#include "logicProcessLayer.h"

/*
worker线程: handleRequest 把 HTML ostream 进 dynamic_body(扩容分配)
         → writeResponse 调 http::async_write(message)
             → beast 现场构造 serializer(分配) → 展平 header+body(拷贝) → 写socket

HTTP 协议不强制要求响应必须由 Beast 产生。
只要发出的字节流正确——状态行、头部、空行、Content-Length 
与实际 body 字节数严格一致——直接 asio::async_write 裸字节完全合法
（Beast 官方 example 里也有 std::string 直接发送的写法）。

所以将对应的回复报文模板化，
绕过beast的自动生成头，和beast的ostream等
*/

// 逻辑处理只做重活
namespace{
    // 通用拼装器：head + Content-Length数字 + 空行 + prefix + 动态数字 + suffix
    // out 必须已 reserve预分配；全程只 append/memcpy + to_chars，不做任何堆分配
    // head : HTTP/1.1 200 OK\r\nContent-Length:
    // body : prefix12345suffix
    void assemble(std::string& out,
                  std::string_view head, // 到 "Content-Length: " 为止的固定部分
                  std::string_view prefix, // body前缀
                  std::uint64_t number,   // body里唯一动态的数字
                  std::string_view suffix) // body后缀
    {
        // 1. 转换动态数字到字符
        char nbuf[24]; // uint64 : 18,446,744,073,709,551,615, 最多20位，24个字符绰绰有余
        // 标准库唯一的"零堆分配"整数格式化,替代原来的 beast::ostream << 数字（ostream 走 locale/streambuf，有分配和虚调用）
        // 将数字 number 高效转换为字符存入 nbuf，并获取转换完成后的尾部指针 nend
        const char* nend = std::to_chars(nbuf, nbuf + 24, number).ptr;
        // 计算 HTTP Body（消息体）的总字节长度：prefix 长度 + 数字字符长度 + suffix 长度
        const std::size_t bodyLen = prefix.size() + (nend - nbuf) + suffix.size();

        // 声明栈上缓冲区，用于存 Content-Length 的数字字符串
        char lbuf[24];
        // 将计算出的 bodyLen 转换为字符存入 lbuf，并获取尾部指针 lend
        const char* lend = std::to_chars(lbuf, lbuf + 24, bodyLen).ptr;
        // 清空输出缓冲区内容(清空size)（保留已分配的内存 capacity，不触发重新分配）
        out.clear();
        out.append(head);
        out.append(lbuf, lend - lbuf);      // Content-Length 的数字
        out.append("\r\n\r\n");             // 头结束空行
        out.append(prefix);                 // 拼接 Body 的前缀 HTML/文本内容
        out.append(nbuf, nend - nbuf);      // 拼接 Body 里的动态数字文本
        out.append(suffix);                 // 拼接 Body 的后缀 HTML/文本内容，完成整个 HTTP 响应报文的拼装
    }

    // 只在"启动期/非热路径"调用，允许分配
    std::string makeStaticResponse(std::string_view status,
                                   std::string_view contentType,
                                   std::string_view body)
    {
        std::string r;
        // 预分配足够存放 HTTP 头部固定文本和 body 内容的内存，减少后续扩容次数
        // 160 为 保守估计的头部长度
        r.reserve(160 + body.size());

        // 拼接协议版本前缀
        r.append("HTTP/1.1");

        // 拼接http状态码及状态描述
        r.append(status);

        // 拼接server头以及 content-Type头字段名
        r.append("\r\nServer: beast\r\nContent-Type: ");

        // 拼接Content-Type的具体值(如tetx/html)
        r.append(contentType);

        // 拼接Content-Length字段名
        r.append("\r\nContent-Length: ");

        // 将 body 长度转为 std::string 并拼接（非热路径，允许一次临时内存分配）
        r.append(std::to_string(body.size()));

        // 拼接Header结束的空行分隔符\r\n\r\n
        r.append("\r\n\r\n");

        // 拼接静态body内容
        r.append(body);

        return r;
    }

    // —— /count 页面文案（与原来 countHtml 相同，拆成三段）——
    constexpr std::string_view kCountHead = "HTTP/1.1 200 OK\r\nServer: beast\r\n"
    "Content-Type: text/html\r\nContent-Length: ";
    constexpr std::string_view kCountPrefix =
    "<html>\n<head><title>Request count</title></head>\n<body>\n"
    "<h1>Request count</h1>\n<p>There have been ";
    constexpr std::string_view kCountSuffix = " requests so far.</p>\n</body>\n</html>\n";

    // —— /time 页面（文案同原 timeHtml，拆成三段）——
    constexpr std::string_view kTimeHead = "HTTP/1.1 200 OK\r\nServer: beast\r\n"
        "Content-Type: text/html\r\nContent-Length: ";
    constexpr std::string_view kTimePrefix =
        "<html>\n<head><title>Current time</title></head>\n<body>\n"
        "<h1>Current time</h1>\n<p>The current time is ";
    constexpr std::string_view kTimeSuffix = " seconds since the epoch.</p>\n</body>\n</html>\n";
} // namespace

logicSystem::logicSystem() : _b_stop(false)
{
    registerCallBacks(); //注册各谓词对应的回调函数
    // 预生成404 响应报文 
    _resp404 = makeStaticResponse("404 Not Found","text/plain","File not found\r\n");

    //for(std::size_t i = 0; i < std::thread::hardware_concurrency();i++){
        _worker_threads.emplace_back([this]{
            this->processRequest();});
        //});
    //}
}

logicSystem::~logicSystem()
{
    _b_stop = true;
    _cond.notify_all();
    
    //std::cout << "logicSystem destructed... " << std::endl;
}

void logicSystem::postRequestToQueue(std::shared_ptr<httpSession> session)
{
    //此处写了个锁外读取信息，导致竟态条件，永远无法唤醒消费者
    // 高并发场景下，队列几乎永不为空，取消was_empty的判断条件

    /* std::unique_lock<std::mutex> locker(_mutex);
    bool was_empty = _requestQueue.empty();
    _requestQueue.push(session); //多线程操作同一个队列，必须加锁

    if(was_empty){ //当队列不为空，立即通知工作线程，消费队列
        _cond.notify_one();
    }
    */
   {
        std::lock_guard<std::mutex> locker(_mutex);
        _requestQueue.push(session);
    }
    _cond.notify_one(); // 锁外通知，避免唤醒后线程立刻在锁上等待
}

void logicSystem::registerCallBacks()
{
    _funcMapping[http::verb::post] = 
        std::bind(&logicSystem::postCallBack,this,std::placeholders::_1);
}

void logicSystem::buildGetResponse(std::shared_ptr<httpSession> session)
{
    const std::string_view target = session->_request.target(); // 获取请求的路径
    std::string& out = session->_sendbuf;   // 将输出缓冲区绑定

    if(target == "/count"){
        // fetch_add 用于原子执行加法,
        // 返回加法前的旧值；本次请求展示的计数 = 旧值+1
        const std::uint64_t n = 
            _requestCount.fetch_add(1,std::memory_order_relaxed) + 1;
        assemble(out,kCountHead,kCountPrefix,n,kCountSuffix);
    }
    else if(target == "/time"){
        assemble(out,kTimeHead,kTimePrefix,
                static_cast<std::uint64_t>(my_program_state::now()),
                kTimeSuffix);
    }else{
        out = _resp404;
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
            // ostream相关方法或许可以想办法绕过
        }
}

void logicSystem::processRequest()
{
    //TODO
    // 此处成为瓶颈
    for(;;){
        std::vector<std::shared_ptr<httpSession>> local_queue;
        local_queue.reserve(128); // 预分配128个槽

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
            
            // 若共享队列不为空，则往线程本地缓存队列里塞
            while(!_requestQueue.empty()){
                local_queue.emplace_back(_requestQueue.front());
                _requestQueue.pop();
            }
        }
        for(auto& session : local_queue){
            handleRequest(session);
            writeResponse(session);
        }
        local_queue.clear();
    }    
}

void logicSystem::handleRequest(std::shared_ptr<httpSession> session){
    // async_read会自动消费buffer，所以不需要手动管理buffer
    // 1. 无论之前状态如何，在开始构造新响应时，第一件事就是清空 response

    
    switch (session->_request.method())
    {
        case http::verb::get:
            buildGetResponse(session);  // get方法走新的字节装配路径，字节会被装进session->_sendbuf中
            break;                      // 此处待修改，应改成重路由消息构建方法
        case http::verb::post:
            session->_response.clear();  // post方法和其他非法方法也应该绕过ostream，自己写报文模板
            session->_response.body().clear();
            session->_response.result(http::status::ok);
            session->_response.set(http::field::server,"beast");
            _funcMapping[http::verb::post](session); //创建回复报文
            break;
        default:
            session->_response.clear();
            session->_response.body().clear();
            session->_response.result(http::status::bad_request); 
            session->_response.set(http::field::content_type,"text/plain"); //设置回复报文类型
            beast::ostream(session->_response.body()) << "Invaild request-method '" //回复错误信息 
            << std::string(session->_request.method_string()) << "'";
            break;
    }
}

void logicSystem::writeResponse(std::shared_ptr<httpSession> session)
{   
    // Get: 字节已装配在_sendBuf,直接裸发送，绕开beast message + serializer
    if (session->_request.method() == http::verb::get) {
        // keep_alive 必须在 _request 被清空之前判定
        session->sendRaw(session->_request.keep_alive());
        return;
    }
    // for post/or other
    session->_response.prepare_payload(); // 自动计算并设置 Content-Length
    if (bool keep_alive = session->_request.keep_alive())
    {
        http::async_write(session->_socket,session->_response,
            [session](beast::error_code ec,std::size_t bytes_transferred)
            {
                if(ec){
                    // 处理写失败
                    beast::error_code ignored_ec;
                    session->_socket.close(ignored_ec);
                    session->_deadline.cancel();
                    if (auto server = session->_server.lock()) {
                        server->get_shardedSessionManager().remove_shard(session->_uuid);
                    } //延长server生命周期
                    return;
                }
                session->_request.clear();
                session->_deadline.expires_after(std::chrono::seconds(60));
                //写成功，重置定时器超时时间,并启动
                session->start(); // 处理下一个请求
            });
    }
    else
    {
        http::async_write(session->_socket,session->_response,
            [session](beast::error_code ec,std::size_t bytes_transferred){
                beast::error_code ignored_ec;
                // 1. 发送 FIN 包
                session->_socket.shutdown(tcp::socket::shutdown_send, ignored_ec);
                // 2. 显式 close 彻底释放 Socket 描述符
                session->_socket.close(ec); //发送完成，服务端主动断开连接
                session->_deadline.cancel(); //消息处理完毕，中止定时器
                session->_server.lock()->get_shardedSessionManager().remove_shard(session->_uuid);
            });
    }
}

//TODO
// 不应该让读和写处于不同的线程执行，会有不必要的上下文切换,用asio::post或asio::distach
// 给 worker 批量加上限（128），让多个 worker 真正并行消费；更进一步做每 worker 私有队列/无锁队列；
// 优化定时器
// 区分动态路径与静态路径，静态直接走模板，动态走逻辑层队列处理
