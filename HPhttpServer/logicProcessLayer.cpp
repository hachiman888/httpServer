#include "logicProcessLayer.h"

logicSystem::logicSystem() : _b_stop(false)
{
    registerCallBacks(); //注册各谓词对应的回调函数
    _worker_thread = std::thread(&logicSystem::processRequest,this); //启动工作线程，处理请求
}

logicSystem::~logicSystem()
{
    _b_stop = true;
    _cond.notify_one();
    _worker_thread.join();
    std::cout << "logicSystem destructed... " << std::endl;
}

void logicSystem::postRequestToQueue(std::shared_ptr<httpSession> session)
{
    bool was_empty = _requestQueue.empty();
    std::unique_lock<std::mutex> locker(_mutex);
    _requestQueue.push(session); //多线程操作同一个队列，必须加锁

    if(was_empty){ //当队列不为空，立即通知工作线程，消费队列
        _cond.notify_one();
    }
}

void logicSystem::registerCallBacks()
{
    _funcMapping[http::verb::get] = 
        std::bind(&logicSystem::getCallBack,this,std::placeholders::_1);
    _funcMapping[http::verb::post] = 
        std::bind(&logicSystem::postCallBack,this,std::placeholders::_1);
}

void logicSystem::getCallBack(std::shared_ptr<httpSession> session) //此处可以修改为向数据库请求页面资源
{
    if(session->_request.target() == "/count"){  //判断要访问的路由是否是服务器所能提供的
            session->_response.set(http::field::content_type,"text/html");
             beast::ostream(session->_response.body()) << "<html>\n"
                << "<head><title>Request count</title></head>\n"
                << "<body>\n"
                << "<h1>Request count</h1>\n"
                << "<p>There have been "
                << my_program_state::request_count()
                << " requests so far.</p>\n"
                << "</body>\n"
                << "</html>\n";
            //创建html格式的回复报文
        }
    else if(session->_request.target() == "/time"){
        session->_response.set(http::field::content_type,"text/html");
        beast::ostream(session->_response.body())
            << "<html>\n"
            << "<head><title>Current time</title></head>\n"
            << "<body>\n"
            << "<h1>Current time</h1>\n"
            << "<p>The current time is "
            << my_program_state::now()
            << " seconds since the epoch.</p>\n"
            << "</body>\n"
            << "</html>\n";
        }
    else //若没找到，则404 not found
    {
        session->_response.result(http::status::not_found);  //设置回复报文的状态码，其状态为404 not found
        session->_response.set(http::field::content_type, "text/plain");
        beast::ostream(session->_response.body()) << "File not found\r\n";
    }
}

void logicSystem::postCallBack(std::shared_ptr<httpSession> session)
{
    if(session->_request.target() == "/email"){
            auto& body = session->_request.body(); // 先取出包体
            auto body_str = beast::buffers_to_string(body.data()); //包体内容转换成string格式,一般会发序列化后的json报文
            std::cout << "receive body is " << body_str << std::endl;
            session->_response.set(http::field::content_type,"text/json"); //打印完后，返回回复报文
            Json::Value root;
            Json::Reader reader;
            Json::Value src_root;
            bool parse_success = reader.parse(body_str,src_root); //将body_str反序列化，存进src_root
            if(!parse_success){
                //若解析失败，则返回一个错误码
                std::cout << "Failed to parse json data..." << std::endl;
                root["error"] = 1001;
                std::string jsonstr = root.toStyledString();
                //写入回复报文包体中
                beast::ostream(session->_response.body()) << jsonstr;
                return;
            }

            //若未发生反序列化失败，则取出发过来的email，处理掉
            auto email = src_root["email"].asString();
            std::cout << "email is " << email << std::endl;
            root["error"] = 0;
            root["email"] = src_root["email"];
            root["msg"] = "receive email post success";
            std::string jsonstr = root.toStyledString();
            beast::ostream(session->_response.body()) << jsonstr; //写入回复报文包体中
        }else{
            session->_response.result(http::status::not_found);  //设置回复报文的状态码，其状态为404 not found
            session->_response.set(http::field::content_type, "text/plain");
            beast::ostream(session->_response.body()) << "File not found\r\n";
        }
}

void logicSystem::processRequest()
{
    for(;;){
        std::shared_ptr<httpSession> session;

        {
            std::unique_lock<std::mutex> locker(_mutex);

             // 条件等待（非停服状态下等待新任务）
            _cond.wait(locker, [this]() {
                return !_requestQueue.empty() || _b_stop; //谓词判断为true时，wait不需要等待信号，直接返回。
            });

            // 停服且队列为空 → 安全退出
            if (_b_stop && _requestQueue.empty()) {
                break;
            }

            // 无论是否停服，只要队列非空就取出处理
            session = _requestQueue.front();
            _requestQueue.pop();
        }
        handleRequest(session);
        writeResponse(session);
    }    
}

void logicSystem::handleRequest(std::shared_ptr<httpSession> session){
    switch (session->_request.method())
    {
        case http::verb::get:
            session->_response.result(http::status::ok); //状态码设置
            session->_response.set(http::field::server,"beast"); //设置服务器名称
            session->_buffer.clear();
            _funcMapping[http::verb::get](session); //创建回复报文
            break;
        case http::verb::post:
            session->_response.result(http::status::ok);
            session->_response.set(http::field::server,"beast");
            session->_buffer.clear();
            _funcMapping[http::verb::post](session); //创建回复报文
            break;
        default:
            session->_response.result(http::status::bad_request); 
            session->_response.set(http::field::content_type,"text/plain"); //设置回复报文类型
            beast::ostream(session->_response.body()) << "Invaild request-method '" //回复错误信息 
            << std::string(session->_request.method_string()) << "'";
            session->_buffer.clear();
            break;
    }
}

void logicSystem::writeResponse(std::shared_ptr<httpSession> session)
{
    bool keep_alive = session->_request.keep_alive();
    if (keep_alive)
    {
        session->_response.content_length(session->_response.body().size());//设置回复报文的包体长度
        http::async_write(session->_socket,session->_response,
            [session](beast::error_code ec,std::size_t bytes_transferred)
            {
                session->_deadline.cancel(); //消息处理完毕，中止定时器
                session->start();
            });
    }
    else
    {
        session->_response.content_length(session->_response.body().size());//设置回复报文的包体长度
        http::async_write(session->_socket,session->_response,
            [session](beast::error_code ec,std::size_t bytes_transferred){
                session->_socket.shutdown(tcp::socket::shutdown_send,ec); //发送完成，服务端主动断开连接
                session->_deadline.cancel(); //消息处理完毕，中止定时器
                asio::post(session->_socket.get_executor(),[session]()
                {
                    session->_server->get_shardedSessionManager().remove_shard(session->_uuid);
                });
            });
    }

}


