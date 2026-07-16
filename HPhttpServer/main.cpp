#include "httpServer.h"
#include "httpSession.h"
#include "IOServicePool.h"
#include <iostream>
#include <boost/asio.hpp>
#include <exception>

int main(){
    try{
        auto pool = IOServicePool::GetInstance();
        auto logicSystem = logicSystem::GetInstance();
        boost::asio::io_context ioc;
        boost::asio::signal_set signals(ioc,SIGINT,SIGTERM);
        signals.async_wait([&ioc,&pool,&logicSystem](auto,auto){
            pool->Stop();
            pool->resetInstance();
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
需要解决程序接收到中断信号后，仍有不明资源未被释放
导致程序无法正常退出的情况
目前观察到发出中断信号后，需要再来一个请求，才能正常退出
*/