#include "httpSession.h"

void httpSession::start(){
    readRequest();
    checkDeadline();
}

//beast本身就已经解决了IO和协议解析(async_read中)，所以只需要分为协议解析层和逻辑处理层即可
void httpSession::readRequest(){
    auto self = shared_from_this();
    http::async_read(_socket,_buffer,_request,
        [self](beast::error_code ec,std::size_t bytes_transferred){
            boost::ignore_unused(bytes_transferred);
            if(!ec){
                // std::cout << "async_read callback: ec=" << ec << " bytes=" << bytes_transferred
                // << " method=" << self->_request.method_string()
                // << " target:" << self->_request.target() << std::endl;
                // 读成功，取消定时器,发送operation_aborted
                self->_deadline.cancel();
                self->processRequest();
            }
            else{
                //对端关闭连接
                if (ec == asio::error::eof || ec == asio::error::connection_reset
                    || ec == http::error::end_of_stream){
                    self->_socket.close();
                    self->_deadline.cancel(); // 关闭定时器
                    if (auto server = self->_server.lock()) {
                        server->_sessionManager.remove_shard(self->_uuid);
                    } //延长server生命周期
                    return;
                }
                else if (ec == asio::error::operation_aborted){
                    self->_socket.close();
                    self->_deadline.cancel(); // 关闭定时器
                    if (auto server = self->_server.lock()) {
                       server->_sessionManager.remove_shard(self->_uuid);
                    } //延长server生命周期
                    return;
                }
                std::cerr << "readRequest error occurred... ec:"
                    << ec << " " << ec.what()<< std::endl;
                self->_socket.close();
                self->_deadline.cancel(); // 关闭定时器
                if (auto server = self->_server.lock()) {
                       server->_sessionManager.remove_shard(self->_uuid);
                } //延长server生命周期
            }
        }); 
}

void httpSession::checkDeadline(){
    auto self = shared_from_this(); 
    
    _deadline.async_wait([self](boost::system::error_code ec){  
        // 1. 拦截取消信号：如果是被 cancel 的，说明有新数据来了，不要关闭 Socket
        if (ec == boost::asio::error::operation_aborted) {
            // 仅仅是定时器被重置/取消了，直接返回
            return; 
        }

        // 2. 拦截其他异常错误
        if (ec) {
            // 可以在这里打印日志或做其他错误处理
            return;
        }

        // 3. 只有真正自然超时（ec == 0），才关闭连接
        boost::system::error_code close_ec;
        self->_socket.close(close_ec); // 用一个局部的 close_ec 接收关闭时的错误，别覆盖了外面的 ec
        if (auto server = self->_server.lock()) {
                       server->_sessionManager.remove_shard(self->_uuid);
        } //延长server生命周期
    });
}

void httpSession::processRequest(){
    auto self = shared_from_this();
    logicSystem::GetInstance()->postRequestToQueue(self);
}