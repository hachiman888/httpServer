#include "httpServer.h"

std::size_t shardedSessionManager::get_shard_index(std::string_view uuid) const
{
    return std::hash<std::string>()(uuid.data()) % _shardsCount;
}

void shardedSessionManager::add_shard(std::shared_ptr<httpSession> session)
{
    //先寻找当前session隶属于哪一片
    const std::size_t shardIndex = get_shard_index(session->getUuid());
    if (shardIndex == -1)
        std::cerr << "add_shard error occurred..." << std::endl;

    auto& shard = _shards[shardIndex];
    //找到后，才进行上锁和插入
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.sessions.insert({session->getUuid(),session});
}

void shardedSessionManager::remove_shard(std::string uuid)
{
    //先找到当前session隶属于哪一片,-1似乎不可能，待修改
    const std::size_t shardIndex = get_shard_index(uuid);
    if (shardIndex == -1)
        std::cerr << "remove_shard error occurred..." << std::endl;

    auto& shard = _shards[shardIndex];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.sessions.erase(uuid);
}

void shardedSessionManager::kill_all(){
    if(_shards.empty()){
        return;
    }
    for (auto& shard : _shards){
        std::lock_guard<std::shared_mutex> guardian(shard.mutex);
        auto sessions = shard.sessions;
        for (auto& session : sessions){
            session.second->getSocket().close();
            session.second->getTimer().cancel();
        }
    }
}

httpServer::httpServer(asio::io_context& ioc,short port_num)
    :_ioc(ioc),_acceptor(_ioc,tcp::endpoint(tcp::v4(),port_num)){}

httpServer::~httpServer()
{
    std::cout << "Server destructed... " << std::endl;
}

shardedSessionManager& httpServer::get_shardedSessionManager() noexcept
{
    return _sessionManager;
}

void httpServer::startListening()
{
    //constexpr int round = 64;
    //for (int i = 0; i < round; ++i)
    //{
        do_Accept();
    //}
}

// 内存分配开销较大，或成为瓶颈，考虑使用socket内存池
void httpServer::do_Accept()
{
    auto& ioc = IOServicePool::GetInstance()->GetIOService(); //从ioc池中获取一个ioc
    std::shared_ptr<httpSession> new_Session = std::make_shared<httpSession>(ioc,shared_from_this()); //根据ioc创建新会话
    _acceptor.async_accept(new_Session->getSocket(),
        std::bind(&httpServer::handle_Accept,this,new_Session,std::placeholders::_1));
    //将新连接和ioc绑定，使得ioc监听其所有异步事件
}

void httpServer::handle_Accept(std::shared_ptr<httpSession> new_Session,
                               const boost::system::error_code& ec)
{
    if(ec){
        std::cerr << "accept error occurred...\terror code is " << ec.value()
            << "error message: " << ec.what();
        return;
        // 正常服务器需要即使在错误发生时，也要持续监听
    }
    new_Session->start(); //启动会话的监听请求逻辑
    _sessionManager.add_shard(new_Session); //将新会话插入红黑树，统一由server管理
    do_Accept();//处理完当前会话连接，递归调用该函数，继续监听连接请求
}

void httpServer::stop_Accept(){
    try{
        this->_acceptor.close();
    }catch (std::exception & e){
        std::cout << "stop Accept error!" << e.what() << std::endl;
    }
}