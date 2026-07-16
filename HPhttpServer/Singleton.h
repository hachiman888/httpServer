//单例类基类
#pragma once
#include <memory>
#include <mutex>
#include <iostream>

template <class T>
class Singleton
{
protected:
    //允许子类访问
    Singleton() = default;
    Singleton(const Singleton<T>&) = delete;
    Singleton& operator=(const Singleton<T>&) = delete;

    //使用static确保实例与程序生命周期一致
    static std::shared_ptr<T> _instance;
public:
    ~Singleton(){
        std::cout << "Singleton instance has been destructed..." 
        << std::endl;
    }

    static std::shared_ptr<T> GetInstance(){
        static std::once_flag _flag; //必须设置为静态变量，不如每次调用都会创建一个新的flag
        std::call_once(_flag,[&](){ //确保仅可调用一次
            _instance = std::shared_ptr<T>(new T);
        });
        return _instance;
    }

    // static void resetInstance(){
    //     _instance.reset(); //强制释放智能指针
    //     std::cout << "shared_ptr has been reset..." << std::endl;
    // }

    void GetAddress(){
        std::cout << _instance->get() << std::endl;
    }
};

template <class T>
std::shared_ptr<T> Singleton<T>::_instance = nullptr; //类外初始化静态成员变量
