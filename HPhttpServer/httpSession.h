#pragma once
#include "httpServer.h"
#include "logicProcessLayer.h"
#include <chrono>
#include <memory>
#include <print>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/reader.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

class httpServer;
class logicSystem;

class httpSession : public std::enable_shared_from_this<httpSession>
{
friend class logicSystem;
public:
    httpSession(asio::io_context& io,std::shared_ptr<httpServer> server)
    :_socket(io),_server(server)
    {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();//生成随机的uuid
        _uuid = boost::uuids::to_string(uuid);
        _sendbuf.reserve(1024);     // 发送缓存仅预分配这一次
    }

    [[nodiscard]] tcp::socket& getSocket() noexcept {
        return _socket;
    }

    [[nodiscard]] std::string getUuid() const noexcept{
        return _uuid;
    }

    [[nodiscard]] asio::steady_timer& getTimer() noexcept{
        return _deadline;
    }

    void start();

    ~httpSession(){
         //std::println("httpSession destructed... uuid: {}", _uuid);
    }

private:
    tcp::socket _socket;                        //存储用于通信的socket
    std::weak_ptr<httpServer> _server;          //方便类内使用map来管理会话,使用weakptr来延长server生命周期，防止server先于session析构
    std::string _uuid;  //用于存储会话的uuid
    beast::flat_buffer _buffer{8192};           //beast库提供的扁平缓冲区
    http::request<http::dynamic_body> _request; //beast库提供的request模板类，期中dynamic body支持各类型请求
    http::response<http::dynamic_body> _response;
    std::string _sendbuf;                       // 裸发送缓冲区 
    asio::steady_timer _deadline{_socket.get_executor(),std::chrono::seconds(60)};  //后续可以 优化为时间轮，避免高并发场景下创建定时器的开销
    //与_socket的调度器绑定，若连接上无任何读写互动，则超时触发回调。

    void readRequest();
    void checkDeadline();
    void processRequest();
    void sendRaw(bool keep_live);
}; 