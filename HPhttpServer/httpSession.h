#pragma once
#include "httpServer.h"
#include "logicProcessLayer.h"
#include <chrono>
#include <memory>
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
    httpSession(asio::io_context& io,httpServer* server)
    :_socket(io),_server(server)
    {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();//生成随机的uuid
        _uuid = boost::uuids::to_string(uuid);
    }

    tcp::socket& getSocket(){
        return _socket;
    }

    std::string getUuid(){
        return _uuid;
    }

    void start();

    ~httpSession(){
        std::cout << "httpSession destructed..." << std::endl;
    }

private:
    tcp::socket _socket; //存储用于通信的socket
    httpServer* _server; //方便类内使用map来管理会话
    std::string _uuid;  //用于存储会话的uuid
    beast::flat_buffer _buffer{8192}; //beast库提供的扁平缓冲区
    http::request<http::dynamic_body> _request; //beast库提供的request模板类，期中dynamic body支持各类型请求
    http::response<http::dynamic_body> _response;
    asio::steady_timer _deadline{_socket.get_executor(),std::chrono::seconds(60)};  //后续可以 优化为时间轮，避免高并发场景下创建定时器的开销
    //与_socket的调度器绑定，若连接上无任何读写互动，则超时触发回调。

    void readRequest();
    void checkDeadline();
    void processRequest();
}; 