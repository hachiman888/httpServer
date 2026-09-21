#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include <type_traits>
#include <chrono>

namespace beast = boost::beast;
namespace http = beast::http;         
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = boost::asio::ip::tcp;

template <typename T>
struct is_ssl_wsstream : std::false_type {};

// 偏特化命中时，只会匹配外层，内部T实际上只是占位符
template <typename T>
struct is_ssl_wsstream<websocket::stream<ssl::stream<T>>> : std::true_type {};

template <typename T>
inline constexpr bool is_ssl_wsstream_v = is_ssl_wsstream<T>::value;

/*
    websocket是基于http的长连接实时双向通信协议
    不该用于静态文件转发
*/

template <typename StreamType>
class Session : public std::enable_shared_from_this<Session<StreamType>>
{
    StreamType ws_;
    beast::flat_buffer buf_;

public:
    explicit Session(StreamType stream)
        : ws_(std::move(stream)){}

    void run(){
        // 使用 net::dispatch 将任务提交到当前 socket 绑定 Executor（通常为 strand）
        // 保证接下来所有的异步操作都在正确的线程上下文/Strand 中被调度
        asio::dispatch(
            ws_.get_executor(),
            beast::bind_front_handler(
                &Session::on_run,
                this->shared_from_this()));
    }

private:
    void on_run(){
        if constexpr(is_ssl_wsstream_v<StreamType>){
            // 如果是 websocket ssl
            // 则先设置握手超时时间，再进行异步握手
            beast::get_lowest_layer(ws_).
                expires_after(std::chrono::seconds(30));

            // 执行 ssl 握手
            ws_.next_layer().async_handshake(
                ssl::stream_base::server,
                beast::bind_front_handler(
                    &Session::on_handshake,
                    this->shared_from_this()));
        }else{
            // 如果是普通的websocket协议，则直接
            // 设置 WebSocket 默认超时参数
            //（例如 Server 模式下的 Ping/Pong 控制周期）
            ws_.set_option(
                websocket::stream_base::timeout::suggested(
                    beast::role_type::server));

            // 设置HTTP装饰器，在握手响应头上加上自定义的“Server”字段
            ws_.set_option(websocket::stream_base::decorator(
                [](websocket::response_type& res){
                    res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING));      
                }
            ));

            // 异步等待并接受客户端发起的 WebSocket 握手请求 (HTTP Upgrade 请求)
            ws_.async_accept(
                beast::bind_front_handler(
                    &Session::on_accept,
                    this->shared_from_this()));
                // 前向绑定器，绑定函数最前面n个参数,从左至右填入
                // 运行时，将产生的参数追加在参数列表后面
                // 类似于std::bind(ph1,ph2,ec,...)
        }
    }

    void on_handshake(beast::error_code ec)
    {
        if(ec){
            // 待报错
            return;
        }

        // 握手后，关闭tcp流的计时器
        // 因为websocket流拥有自己的计时系统
        beast::get_lowest_layer(ws_).expires_never();

        // 设置 WebSocket 默认超时参数
        //（例如 Server 模式下的 Ping/Pong 控制周期）
        ws_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::server));

        // 设置装饰器，在握手响应头上加上自定义的“Server”字段
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res)
            {
                res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING));
            }));
        
        // ssl握手成功，异步等待并接受客户端发起的 
        // WebSocket 握手请求 (HTTP Upgrade 请求)
        ws_.async_accept(
            beast::bind_front_handler(
                &Session::on_accept,
                this->shared_from_this()));
    }

    void on_accept(beast::error_code ec)
    {
        if(ec){
            // 待报错
            return;
        }

        // websocket握手成功，开始双向通信
        do_read();
    }

    void do_read(){
        // 异步等待客户端发送 WebSocket 消息（帧数据）
        ws_.async_read(
            buf_,
            beast::bind_front_handler(
                &Session::on_read,
                this->shared_from_this()
            ));
    }

    void on_read(beast::error_code ec,std::size_t bytes_transferred)
    {   
        // 无视无用参数，避免警告
        boost::ignore_unused(bytes_transferred);

        if(ec == websocket::error::closed)
            return;
        
        if(ec){
            // 待报错
            return;
        }

        // 设置输出消息类型（文本类型还是二进制类型，与收到的消息保持一致）
        ws_.text(ws_.got_text());

        // 异步将接收到的 buffer_ 里的数据原样写回给客户端
        ws_.async_write(
            buf_.data(),
            beast::bind_front_handler(
                &Session::on_write,
                this->shared_from_this()));
    }

    void on_write(beast::error_code ec,std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec){
            // 待报错
            return;
        }

        // 清理缓冲区
        buf_.consume(buf_.size());

        // 再次读
        do_read();
    }
};

using wsSession = Session<websocket::stream<beast::tcp_stream>>;
using wssSession = Session<websocket::stream<ssl::stream<beast::tcp_stream>>>;