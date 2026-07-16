#include "httpServer.h"
#include "httpSession.h"
#include "IOServicePool.h"
#include <iostream>
#include <boost/asio.hpp>
#include <exception>

int main(){
    try{
        auto pool = IOServicePool::GetInstance();
        boost::asio::io_context ioc;
        boost::asio::signal_set signals(ioc,SIGINT,SIGTERM);
        signals.async_wait([&ioc,&pool](auto,auto){
            pool->Stop();
            //pool->resetInstance();
            ioc.stop();
            std::cout << "ioc stopped..." << std::endl;
        });

        httpServer s(ioc,8080);
        ioc.run();
    }catch(std::exception& e){
        std::cerr << "error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

/*
 @TODO
 当用户尚未关闭浏览器时，服务器接收到中止信号后
 会持续等待定时器超时，才能正常析构
 主要体现在逻辑处理层需要等待定时器超时，才能析构
*/