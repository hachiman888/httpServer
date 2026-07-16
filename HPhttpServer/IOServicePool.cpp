#include "IOServicePool.h"

IOServicePool::IOServicePool(std::size_t size)
    :_IOServices(size),_works(size),_nextIOService(0)
{
    for(std::size_t i = 0;i < size;i++){
        //将ioc和work对象一对一绑定,以确保ioc在空闲时，不会提前析构
        _works[i] = std::make_unique<Work>(_IOServices[i]);
    }
    _threads.reserve(size);
    for(std::size_t i = 0;i < size;i++){
        _threads.emplace_back(std::jthread([this,i](){
            //若work析构，则run在空闲时退出，ioc析构，线程退出
            _IOServices[i].run();
        }));
    }
}

IOServicePool::~IOServicePool(){
    std::cout << "IOServicePool has been destructed..." << std::endl;
}

boost::asio::io_context& IOServicePool::GetIOService(){
    auto& result = _IOServices[_nextIOService++]; //轮询回环
    if(_nextIOService == _IOServices.size()){
        _nextIOService = 0;
    }

    return result;
}

void IOServicePool::Stop(){
    for(auto& work:_works){
        //析构各work，使得ioc在空闲时，退出run函数
        work.reset();
    }
    std::cout << "all works have been deleted... " << std::endl;
}