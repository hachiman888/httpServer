#pragma once
#include "Singleton.h"
#include <boost/asio.hpp>
#include <thread>
#include <vector>

class IOServicePool : public Singleton<IOServicePool>
{
friend class Singleton<IOServicePool>;
public:
    using IOService = boost::asio::io_context;
    using Work = boost::asio::io_context::work;
    using WorkPtr = std::unique_ptr<Work>;

    IOService& GetIOService(); //用轮询的方式获取ioc
    void Stop(); //停止ioc池的函数

    //允许移动，不允许复制
    ~IOServicePool();
    IOServicePool(const IOServicePool&) = delete;
    IOService& operator=(const IOServicePool&) = delete;
private:
    IOServicePool(std::size_t size = std::thread::hardware_concurrency() / 2 );
    std::vector<IOService> _IOServices; //初始化多个ioc
    std::vector<WorkPtr> _works; //用于和多个ioc配对的work对象
    std::vector<std::jthread> _threads; //用于管理所有线程
    std::size_t  _nextIOService; //表示需要返回的下一个IOService的索引
};