#include "IOServicePool.h"
#include "shared_object_pool.hpp"
#include "httpSessionTemplate.hpp"
#include "http_httpsServer.hpp"
#include "staticFileCache.hpp"
#include <iostream>
#include <boost/asio.hpp>
#include <exception>

// #define BOOST_ASIO_HAS_IO_URING

static inline constexpr std::string root = ".";

int main(){
    try{
        boost::asio::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_server}; // 若测试ssl，还需要加载ssl证书，详见官方示例

       auto root_ptr = std::make_shared<const std::string>(root);

       auto server = std::make_shared<Server<beast::tcp_stream>>(ioc,8080,ctx,root_ptr);
       server->run();
       ioc.run(); // 程序在此阻塞，无需关心site的生命周期
    }catch(std::exception& e){
        std::cerr << e.what() << "\n";
    }
}