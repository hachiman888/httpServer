#include "httpServer.h"

httpServer::httpServer(asio::io_context& io,short port_num)
    :_ioc(io),_acceptor(_ioc,tcp::endpoint(tcp::v4(),port_num))
{
    start_Accept();
}

void httpServer::clearSession(std::string uuid)
{
    std::cout << "Session  cleared..." << std::endl;
        _sessions.erase(uuid); //若uuid不存在，则无事发生
}

httpServer::~httpServer()
{
    std::cout << "Server destructed... " << std::endl;
}

void httpServer::start_Accept(){
    auto& ioc = IOServicePool::GetInstance()->GetIOService(); //从ioc池中获取一个ioc
    std::shared_ptr<httpSession> new_Session = std::make_shared<httpSession>(ioc,this); //根据ioc创建新会话
    _acceptor.async_accept(new_Session->getSocket(),
        std::bind(&httpServer::handle_Accept,this,new_Session,std::placeholders::_1));  
        //将新连接和ioc绑定，使得ioc监听其所有异步事件 
}

void httpServer::handle_Accept(std::shared_ptr<httpSession> new_Session,
        const boost::system::error_code& ec)
{
    if(ec){
        std::cerr << "accept error occurred...\terror code is " << ec.value()
            << "error message: " << ec.what();
        return;
    }
    new_Session->start(); //启动会话的监听请求逻辑
    _sessions.insert({new_Session->getUuid(),new_Session}); //将新会话插入红黑树，统一由server管理

    start_Accept();//处理完当前会话连接，递归调用该函数，继续监听连接请求
}