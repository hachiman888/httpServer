#include "ws&wssServer.hpp"
#include "wsSessionTemplate.hpp"
#include <exception>
#include <iostream>
#include <vector>
#include <thread>
#include <cstddef>

int main(){
    try{
        asio::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_server};
        asio::ip::tcp::endpoint ep(tcp::v4(),8089);

        auto server = std::make_shared<Server<websocket::stream<beast::tcp_stream>>>(ioc,ctx,ep);
        // 若使用wss协议，则需要生成ssl证书，且需要把证书加载进ctx中
        // 详细需要查看beast官方例子
        server->run();

        std::vector<std::thread> v;
        v.reserve(std::thread::hardware_concurrency()/2);
        for(size_t i = 0; i < std::thread::hardware_concurrency()/2;++i)
        {
            v.emplace_back([&ioc]{
                ioc.run();
            });
        }

        ioc.run();
    }catch(std::exception& e){
        std::cerr << e.what() << "\n";
    }
}

// TODO
// 待对ws服务器进行压力测试