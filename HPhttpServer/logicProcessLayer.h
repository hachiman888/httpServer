#pragma once
#include "httpSession.h"
#include "Singleton.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <thread>
#include <queue>
#include <map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

namespace my_program_state{
    static inline std::size_t request_count(){  //用于统计有多少请求
        static std::size_t count = 0;
        return ++count;
    }

    static inline std::time_t now(){
        return std::time(0);//返回当前时间戳
    }
}

class httpSession;

class logicSystem : public Singleton<logicSystem>
{
friend class Singleton<logicSystem>;
using CallBack = std::function<void(std::shared_ptr<httpSession>)>;
public:
    ~logicSystem();
    void postRequestToQueue(std::shared_ptr<httpSession> session); //将请求投入待处理队列

private:
    logicSystem();
    void registerCallBacks();
    void getCallBack(std::shared_ptr<httpSession>);
    void postCallBack(std::shared_ptr<httpSession>);
    void processRequest();
    void handleRequest(std::shared_ptr<httpSession>);
    void writeResponse(std::shared_ptr<httpSession>);

    std::queue<std::shared_ptr<httpSession>> _requestQueue;
    bool _b_stop;
    std::mutex _mutex;
    std::condition_variable _cond;
    std::thread _worker_thread;
    std::map<http::verb,CallBack> _funcMapping;
};

