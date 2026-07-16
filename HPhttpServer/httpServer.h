#pragma once
#include "IOServicePool.h"
#include "httpSession.h"
#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <memory>
#include <map>
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

class httpSession;

class httpServer{
public:
    httpServer(boost::asio::io_context& ioc,short port_num);
    void clearSession(std::string uuid);
    ~httpServer();

private:
    void start_Accept();
    void handle_Accept(std::shared_ptr<httpSession> new_session,const boost::system::error_code& ec);
    boost::asio::io_context& _ioc; //不可复制，只可移动
    boost::asio::ip::tcp::acceptor _acceptor;
    std::map<std::string,std::shared_ptr<httpSession>> _sessions; //用于管理连接会话
};
