#pragma once

#include "logicProcessLayer.h"
#include "staticFileCache.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <variant>
#include <memory>
#include <type_traits>
#include <array>      // response_view 里的 std::array<asio::const_buffer,2>
#include <cstddef>    // std::size_t

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

// 辅助函数 ：拼接根目录路径与客户端请求的相对路径，并处理跨平台路径分隔符（Windows '\' vs Linux '/'）
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

// ============================================================================
//  【改动 A-1】新增 response_view —— 缓存命中的响应用"两段 buffer 的视图"表示
// ----------------------------------------------------------------------------
//  为什么要这个类型：
//    缓存命中时，响应其实是两块**已经存在于内存里、且不会变**的字节：
//        [0] 启动期预拼好的响应头（staticFileCache::Entry::head_keep_alive / head_close）
//        [1] 文件内容本体（staticFileCache::Entry::body）
//    旧写法是新建一个 std::string，把头、体全量拷进去再发送 ——
//    每请求一次 malloc + 一次全量 body 的 memcpy（文件越大，这笔拷贝越是主导开销）。
//
//  这个类型只存**指针**，不存字节：拷贝它等于拷贝两个 (ptr,size) 对，零开销。
//  配合下面的 do_write，asio 会把这两段用一次 sendmsg（msg_iov 聚集写，等价于 writev）
//  发出去 —— 仍然只是一个系统调用，且不拷贝 body。
//
//  两个必须记住的点：
//    1) 生命周期：它指向的内存必须活到异步写完成。这里指向的是 staticFileCache
//       里的常驻内容（进程级生命期），所以天然安全；
//       —— 这一点正是旧 std::string 版本的隐患所在，见下面 do_write 里的说明。
//    2) 它只对"内容固定"的响应成立。动态生成的响应（要现场拼数字/时间的）不适用，
//       那种情况应该用一块可复用的 session 缓冲区（clear() 保留 capacity 的写法）。
// ============================================================================
struct response_view
{
    // 最多两段：头 + 体。HEAD 请求时只发头（count = 1）
    std::array<asio::const_buffer, 2> bufs{};
    std::size_t count = 0;

    // 下面这两个 begin/end 让 response_view 自身满足 asio 的 ConstBufferSequence 概念，
    // 于是可以直接 asio::async_write(stream, view, handler)，
    // 不用再拼 asio::buffer(...)（asio 也没有 "const_buffer* + 个数" 这种重载）。
    [[nodiscard]] const asio::const_buffer* begin() const noexcept { return bufs.data(); }
    [[nodiscard]] const asio::const_buffer* end()   const noexcept { return bufs.data() + count; }

    // 惯用的 typedef，让 asio 的 buffer 序列萃取能正常工作
    using value_type = asio::const_buffer;
};

// 处理客户端发来的 HTTP 请求时，未命中文件缓存,则走这个路径，生成相应的 Response 报文
// message_generator 是 Beast 提供的类型擦除包装器，可统一返回不同 Body 类型的 Response
//
// 【改动 A-2】返回类型里原来的 std::string 变成了 response_view：
//   现在"缓存命中"不再构造字符串，而是返回一个指向缓存内容的视图（零拷贝、零分配）；
//   其余所有分支仍然返回 beast 的 response，会隐式转成 message_generator 这个备选类型。
template<class Body,class Allocator>
std::variant<response_view,http::message_generator> handle_request(
    std::string_view doc_root,
    http::request<Body,http::basic_fields<Allocator>>&& req,
    const hp::staticFileCache& fileCache)    
{
    // 检验1： 只支持GET,HEAD,和POST方法
    if( req.method() != http::verb::get &&
        req.method() != http::verb::head)
        return bad_request("Unknown HTTP-method",req);

    // 检验2：请求路径合法性安全检查(防止路径穿越攻击，如 GET /../etc/passwd)
    if( req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != beast::string_view::npos)
        return bad_request("Illegal request-target",req);

    // 判断文件缓存是否命中，未命中才走旧流程
    auto target = req.target();
    if(auto entry = fileCache.find(target);entry != nullptr)
    {   
        // std::cout << "缓存命中..." << std::endl;

        // 【改动 A-3】命中后不再拼接字符串，而是把"预拼好的响应头"和"文件内容"
        // 各自作为一个 const_buffer 指出去：
        //   * 两块内存都属于 staticFileCache::Entry，进程级生命期，写的时候一定还在；
        //   * 不 malloc、不 memcpy，拷贝的只是两个 (指针,长度) 对。
        const std::string& head = req.keep_alive()
                                ? entry->head_keep_alive
                                : entry->head_close;

        response_view view;
        // asio::buffer 本质上就是一个轻量级的内存视图（View）
        view.bufs[0] = asio::buffer(head);         // 第一段：响应头
        view.bufs[1] = asio::buffer(entry->body);  // 第二段：文件内容

        // HEAD 只发响应头（count = 1）。Content-Length 仍然是文件的真实大小，
        // 这是 RFC 要求的：HEAD 的 Content-Length 表示"如果发 GET 会收到多少字节"。
        // 注意：这里如果不区分 HEAD，body 会被一起发出去，长连接下客户端会把
        //       下一段响应的字节当成这个 body 来解析，整条连接的报文就错位了。
        view.count = (req.method() == http::verb::head) ? 1u : 2u;

        return view;
    }    

    // 构建本地文件的绝对路径；若访问根目录，默认指向 index.html
    std::string path = path_cat(doc_root, target);
    if(req.target().back() == '/')
        path.append("index.html");

    // 尝试打开目标静态文件
    // 此处待修改成访问服务器缓存
    beast::error_code ec;
    http::file_body::value_type body;
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
        res.set(http::field::content_type, hp::mime_type_of(path));
        res.content_length(size);
        res.keep_alive(req.keep_alive());
        return res;
    }

    // 静态文件转发服务器，禁用post
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
    res.set(http::field::content_type, hp::mime_type_of(path));
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
    const hp::staticFileCache& cache_;
    
public:
    explicit Session(Stream stream,
                     std::shared_ptr<std::string const> const& doc_root,
                     const hp::staticFileCache& file_cache) 
        : stream_(std::move(stream))
          ,doc_root_(doc_root)
          ,cache_(file_cache)
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
                        // 待报错
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
            // 待报错
            return;
        }

        // 无事发生，则继续执行
        do_write(
            handle_request(*doc_root_,std::move(request_),cache_));
    }

    // 【改动 A-4】形参类型跟着 handle_request 的返回类型走：string -> response_view
    void do_write(std::variant<response_view,http::message_generator>&& msg)
    {   
       // 使用std::vist 搭配完美转发，保证msg属性
       std::visit([this](auto&& arg){
        // arg会自动获取msg中的类型的引用（左/右）
        // 类型萃取,剥离&或&&，获得裸类型
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T,http::message_generator>){
            // arg 的类型此时就是 http::message_generator&
            //（如果外层传了 move，这里就是&&）
            bool keep_alive = arg.keep_alive();
            
            // 需要用beast的异步写
            beast::async_write(stream_, std::move(arg),
                beast::bind_front_handler(
                    &Session::on_write,
                    this->shared_from_this(),
                    keep_alive));
        }
        else if constexpr(std::is_same_v<T,response_view>){
            // 长短连接由请求决定（这里 request_ 还是本次请求，没被下一次读覆盖）
            bool keep_alive = request_.keep_alive();

            // 【改动 A-5】两段 buffer 一次性写出去：单段 buffer 时 asio 走 send()，
            // 多段时走 sendmsg()（msg_iov 聚集写，等价于 writev）—— 都只是**一个**系统调用，
            // 全程不拷贝、不分配 body。
            //
            // 这里为什么安全 —— 对比一下被替换掉的旧写法：
            //   旧：asio::async_write(stream_, asio::buffer(arg.data(), arg.size()), ...)
            //       arg 是 msg 里那个 std::string，而 msg 绑定的是 handle_request(...)
            //       返回的**临时** variant；asio 的异步写内部只保存了 buffer 的**指针**，
            //       并不拷贝数据。于是这个 full-expression(异步写) 一结束、variant 析构、string 的
            //       堆内存 free 之后，异步写还拿着悬垂指针 —— 只要响应大到一次写不完
            //       （需要第二次 write_some），那次写就会去读已释放的内存，是真正的 UAF。
            //       这个机制我用最小复现确认过：socketpair + 强制很小的 SO_SNDBUF +
            //       发起 async_write 后立刻 delete[]，ASan 报
            //       "heap-use-after-free ... READ of size 8064 in send()"。
            //       注意它是**潜在**的：响应小到一次 send() 就能写完时不会触发，
            //       所以平时看不出来；大文件、或对端读得慢（发送缓冲填满）时才会真的踩到。
            //   新：arg 只是"视图"，它指向的字节在 staticFileCache 里，活到进程结束。
            //       视图对象本身会被 asio 拷进组合操作内部，所以放栈上（这里在 variant 里）
            //       也完全没问题 —— 需要长期存活的是**被指向的内存**，不是视图本身。
            asio::async_write(stream_, arg,
                beast::bind_front_handler(
                    &Session::on_write,
                    this->shared_from_this(),
                    keep_alive));
        }
       },std::move(msg));
    }

    void on_write(bool keep_alive,beast::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        if(ec){
            // 待报错
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
                if(ec){
                    // 待报错 
                    return; 
                }
            });
        }else{
            beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(stream_).socket().close(ec);
        }
    }
};

using http_session = Session<beast::tcp_stream>;
using https_session = Session<ssl::stream<beast::tcp_stream>>;