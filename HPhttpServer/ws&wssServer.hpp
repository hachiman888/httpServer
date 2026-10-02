#pragma once
#include "wsSessionTemplate.hpp"

template <class StreamType>
class Server : public std::enable_shared_from_this<Server<StreamType>>
{
    asio::io_context& ioc_;
    ssl::context& ctx_;
    tcp::acceptor acceptor_;

public:
    Server(asio::io_context& ioc,
           ssl::context& ctx,
           tcp::endpoint endpoint)
           : ioc_(ioc)
           , ctx_(ctx)
           , acceptor_(asio::make_strand(ioc))
           // 将acceptor与ioc的strand绑定
           // 防止多个线程同时操作acceptor的相关事件
    {
        beast::error_code ec;

        // 打开socket监听器
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor open failed");
        }

        // 在 bind 之前设置端口复用 (此时有效)
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw boost::system::system_error(ec, "set_option reuse_address failed");
        }

        // 显式绑定端口
        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor bind failed");
        }

        // 启动监听
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor listen failed");
        }
    }

    void run(){
        do_accept();
    }

private:
    void do_accept(){
        // 新连接的socket绑定到自己唯一的，
        // 绑定到ioc_的strand的执行器上
        acceptor_.async_accept(
            asio::make_strand(ioc_),
            beast::bind_front_handler(
                &Server::on_accept,
                this->shared_from_this()
            ));
        // 前向绑定器，绑定函数最前面n个参数,从左至右填入
        // 运行时，将产生的参数追加在参数列表后面
        // 类似于std::bind(ph1,ph2,ec,socket)
    }

    void on_accept(beast::error_code ec,tcp::socket socket)
    {
        if(ec){
            // 待报错
            return;
        }

        if constexpr(is_ssl_wsstream_v<StreamType>){
            websocket::stream<ssl::stream<beast::tcp_stream>> 
                stream(std::move(socket),ctx_);
            std::make_shared<wssSession>(std::move(stream))->run();
        }else{
            websocket::stream<beast::tcp_stream> 
                stream(std::move(socket));
            std::make_shared<wsSession>(std::move(stream))->run();
        }

        // 下一轮监听
        do_accept();
    }
};