#pragma once
#include "IOServicePool.h"
#include "httpSession.h"
#include <boost/asio.hpp>
#include <shared_mutex>
#include <vector>
#include <iostream>
#include <string>
#include <memory>
#include <map>
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

class httpSession;

class shardedSessionManager
//分片锁
{
public:
    //根据ioc池中的ioc数量来决定分多少个片
    explicit shardedSessionManager
        (std::size_t shardsCount = std::thread::hardware_concurrency()) noexcept:
        _shardsCount(shardsCount),_shards(shardsCount){}

    [[nodiscard]]std::size_t get_shard_index(std::string_view uuid) const;

    //交由acceptor所在的线程调用
    void add_shard(std::shared_ptr<httpSession> session);

    //交由session所隶属的ioc调用
    void remove_shard(std::string uuid);

    void kill_all();


private:
    struct Shard
    {
        mutable std::shared_mutex mutex;
        std::map<std::string,std::shared_ptr<httpSession>> sessions;
    };

    std::size_t _shardsCount;
    std::vector<Shard> _shards;
};

class httpServer : public std::enable_shared_from_this<httpServer>{
    friend class httpSession;
public:
    httpServer(asio::io_context& ioc,short port_num);
    ~httpServer();
    shardedSessionManager& get_shardedSessionManager() noexcept;
    void startListening();
    void stop_Accept();

private:

    void do_Accept();
    void handle_Accept(std::shared_ptr<httpSession> new_session,const boost::system::error_code& ec);
    asio::io_context& _ioc; //不可复制，只可移动
    ip::tcp::acceptor _acceptor;
    shardedSessionManager _sessionManager;
};
