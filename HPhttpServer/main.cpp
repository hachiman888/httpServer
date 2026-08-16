#include "httpServer.h"
#include "httpSession.h"
#include "IOServicePool.h"
#include <iostream>
#include <boost/asio.hpp>
#include <exception>
#include <gperftools/profiler.h>


int main(){
    try{
        ProfilerStart("cpu.prof");
        boost::asio::io_context ioc;
        // 对于enable_shared_from_this 内部的那个 weak_ptr 只有在 make_shared 返回、shared_ptr 真正接管对象的那一刻才被初始化。
        // 构造函数还在执行时，对象尚未被任何 shared_ptr 持有，此时调用 shared_from_this() 在 C++17 起是未定义行为，
        // libstdc++ 的实现表现为抛出 std::bad_weak_ptr。
        // 因此，需要拆分server构造函数和session构造函数的相关性，解耦合
        auto server = std::make_shared<httpServer>(ioc,8081);
        server->startListening();
        auto pool = IOServicePool::GetInstance();

        boost::asio::signal_set signals(ioc,SIGINT,SIGTERM);
        signals.async_wait([&ioc,&pool,server](auto,auto){
            server->stop_Accept();
            auto& sessionManager = server->get_shardedSessionManager();
            sessionManager.kill_all();
            pool->Stop();
            ioc.stop();
            ProfilerStop();
            std::cout << "all stopped..." << std::endl;
        });
        ioc.run();
    }catch(std::exception& e){
        std::cerr << "error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

/*TODO
 * 并发能力有待提高
 * 首先需要把调试用的日志去掉
 * 拓展逻辑系统的工作线程，避免队列过长导致排队时间上升
 * 
*/