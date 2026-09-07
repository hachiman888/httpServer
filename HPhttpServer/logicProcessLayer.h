#pragma once
#include "httpSession.h"
#include "Singleton.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <queue>
#include <map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

namespace my_program_state{
    static inline std::time_t now(){
        return std::time(0);//返回当前时间戳
    }
}

class httpSession;

class logicSystem : public Singleton<logicSystem>
{
friend class Singleton<logicSystem>;
friend class httpSession;
using CallBack = std::function<void(std::shared_ptr<httpSession>)>;
public:
    ~logicSystem();
    void postRequestToQueue(std::shared_ptr<httpSession> session); //将请求投入待处理队列

private:
    logicSystem();
    void registerCallBacks();
    void buildGetResponse(std::shared_ptr<httpSession> session); // GET 路由分流+装配
    void postCallBack(std::shared_ptr<httpSession>);
    void processRequest();
    void handleRequest(std::shared_ptr<httpSession>);
    void writeResponse(std::shared_ptr<httpSession>);
    
    bool _b_stop;
    std::mutex _mutex;
    std::condition_variable _cond;
    std::vector<std::jthread> _worker_threads;
    std::map<http::verb,CallBack> _funcMapping;
    std::atomic<std::uint64_t> _requestCount{0};  // 用于count计数，面向多线程，必须原子
    std::string _resp404;                         // 启动时预生成的完整 404 响应字节
    std::queue<std::shared_ptr<httpSession>> _requestQueue;
};

