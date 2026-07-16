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

