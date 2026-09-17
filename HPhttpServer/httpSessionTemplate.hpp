#pragma once
#include "httpServer.h"
#include "logicProcessLayer.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/reader.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <memory>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ip = asio::ip;
namespace ssl = asio::ssl;
using tcp = ip::tcp;

 // 生成 400 Bad Request 响应的函数
static inline const auto bad_request = []
    (std::string_view why,auto& req)
    {
        http::response<http::string_body> res{http::status::bad_request,req.version()};
        res.set(http::field::server,BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type,"text/html");
        res.keep_alive(req.keep_alive());
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

// 生成 404 Not Found 响应的函数
static inline const auto not_found =
    [](std::string_view target,auto& req)
    {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "The resource '" + std::string(target) + "' was not found.";
        res.prepare_payload();
        return res;
    };

// 生成 500 Internal Server Error 响应的函数
static inline const auto server_error =
    [](std::string_view what,auto& req)
    {
        http::response<http::string_body> res{http::status::internal_server_error, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "An error occurred: '" + std::string(what) + "'";
        res.prepare_payload();
        return res;
    };

// 辅助函数 1：根据文件扩展名返回对应的 MIME 类型（用于设置 HTTP Content-Type）
beast::string_view mime_type(beast::string_view path)
{
    using beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if(pos == beast::string_view::npos)
            return beast::string_view{};
        return path.substr(pos);
    }();
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".php"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".swf"))  return "application/x-shockwave-flash";
    if(iequals(ext, ".flv"))  return "video/x-flv";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/vnd.microsoft.icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/octet-stream";
}

// 辅助函数 2：拼接根目录路径与客户端请求的相对路径，并处理跨平台路径分隔符（Windows '\' vs Linux '/'）
std::string path_cat(beast::string_view base,
    beast::string_view path)
{
    if(base.empty())
        return std::string(path);
    std::string result(base);
#ifdef BOOST_MSVC
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

// 业务核心函数：处理客户端发来的 HTTP 请求并生成相应的 Response 报文
// message_generator 是 Beast 提供的类型擦除包装器，可统一返回不同 Body 类型的 Response
// TODO
// 每请求都打开关闭某个文件，path_cat 字符串构造、mime 比较、message_generator 堆分配，形成瓶颈
// 给静态文件做内存缓存（启动时读进 std::string，或缓存 fd），
// 热路由回复预拼装成字节（就是把 main 里 assemble() 那套 reserve + to_chars 搬过来）。
template<class Body,class Allocator>
http::message_generator handle_request(
    std::string_view doc_root,
    http::request<Body,http::basic_fields<Allocator>>&& req)    
{
    // 检验1： 只支持GET,HEAD,和POST方法
    if( req.method() != http::verb::get &&
        req.method() != http::verb::head &&
        req.method() != http::verb::post)
        return bad_request("Unknown HTTP-method",req);

    // 检验2：请求路径合法性安全检查(防止路径穿越攻击，如 GET /../etc/passwd)
    if( req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != beast::string_view::npos)
        return bad_request("Illegal request-target",req);

    // 构建本地文件的绝对路径；若访问根目录，默认指向 index.html
    std::string path = path_cat(doc_root, req.target());
    if(req.target().back() == '/')
        path.append("index.html");

    // 尝试打开目标静态文件
    // 此处待修改成访问服务器缓存
    beast::error_code ec;
    http::file_body::value_type body;
    // http::string_body::value_type s_body;
    body.open(path.c_str(), beast::file_mode::scan, ec);

    // 处理文件不存在的情况 (404)
    if(ec == beast::errc::no_such_file_or_directory)
        return not_found(req.target(),req);

    // 处理其他系统读取错误 (500)
    if(ec)
        return server_error(ec.message(),req);

    // 缓存文件体积大小
    auto const size = body.size();

    // 如果是 HEAD 请求，只返回响应头，不附带文件内容
    if(req.method() == http::verb::head)
    {
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(path));
        res.content_length(size);
        res.keep_alive(req.keep_alive());
        return res;
    }

    // 暂定
    /*
    if(req.method() == http::verb::post)
    {
        return res{};
    }
    */

    // 如果是 GET 请求，构造包含文件流 (file_body) 的 200 OK 响应
    http::response<http::file_body> res{
        std::piecewise_construct,
        std::make_tuple(std::move(body)),
        std::make_tuple(http::status::ok, req.version())};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, mime_type(path));
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return res;
}

// 主模板，默认所有类型都不是ssl_stream类型
template <typename T>
struct is_ssl_stream : std::false_type {}; 

// 偏特化
template <typename T>
struct is_ssl_stream<ssl::stream<T>> : std::true_type {};

template <class T>
inline constexpr bool is_ssl_stream_v = is_ssl_stream<T>::value;

template <class Stream>
class Session : public std::enable_shared_from_this<Session<Stream>>
{
    Stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    std::shared_ptr<std::string const> doc_root_;
    
public:
    explicit Session(Stream stream,
        std::shared_ptr<std::string const> const& doc_root) 
        : stream_(std::move(stream)),
          doc_root_(doc_root)
        {}

    void run()
    {   
        // 给session对应ioc投递任务
        asio::dispatch(
            stream_.get_executor(),
            beast::bind_front_handler(
                &Session::on_run,
                this->shared_from_this()));    
    }

    void on_run()
    {
        if constexpr(is_ssl_stream_v<Stream>){
            // 如果是https_session,则tcp连接结束之后还需要TLS握手
            auto self = this->shared_from_this();

            //设置定时器
            beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
            stream_.async_handshake(ssl::stream_base::server,
                [self](beast::error_code ec){
                    if(ec) {
                        return;
                    }
                    self->do_read();
                });
        }else{
            do_read();
        }
    }

    void do_read()
    {
        request_ = {};

        // 重置定时器
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        http::async_read(stream_,buffer_,request_,
            beast::bind_front_handler(&Session::on_read,this->shared_from_this()));
    }

    void on_read(boost::system::error_code ec,std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if(ec == asio::error::eof || ec == asio::error::connection_reset
            || ec == http::error::end_of_stream)
        {
            return do_close();
        }

        if(ec){
            return;
        }

        // 无事发生，则继续执行
        do_write(
            handle_request(*doc_root_,std::move(request_)));
    }

    void do_write(http::message_generator&& msg)
    {
        bool keep_alive = msg.keep_alive();

        // 用beast的异步写
        beast::async_write(stream_,std::move(msg),
            beast::bind_front_handler(
                &Session::on_write,
                this->shared_from_this(),
                keep_alive));
    }

    void on_write(bool keep_alive,beast::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if(ec){
            return;
        }

        // 非长连接，则断开
        if(!keep_alive){
            return do_close();
        }

        // 为长连接，继续读
        do_read();
    }
    
    void do_close(){
        beast::error_code ec;
        beast::get_lowest_layer(stream_).expires_never();

        if constexpr(is_ssl_stream_v<Stream>){
            // TLS 主动关闭协议
            beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
            
            auto self = this->shared_from_this();
            stream_.async_shutdown([self](beast::error_code ec)
            {
                if(ec){ return; }
            });
        }else{
            beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(stream_).socket().close(ec);
        }
    }
};

using http_session = Session<beast::tcp_stream>;
using https_session = Session<ssl::stream<beast::tcp_stream>>;