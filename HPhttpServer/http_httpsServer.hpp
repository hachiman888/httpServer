#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <memory>
#include <string>
#include "httpSessionTemplate.hpp"
#include "shared_object_pool.hpp"
#include "IOServicePool.h"

namespace asio = boost::asio;
namespace ssl = asio::ssl;
namespace ip = asio::ip;
using tcp = ip::tcp;

template<class StreamType>
class Server : public std::enable_shared_from_this<Server<StreamType>>
{
    asio::io_context& ioc_;
    ssl::context& ctx_;
    tcp::acceptor acceptor_;
    std::shared_ptr<const std::string>doc_root_;

public:
    Server(asio::io_context& ioc,
           short port_num,
           ssl::context& ctx,
           std::shared_ptr<const std::string> const& doc_root)
           :ioc_(ioc)
           ,ctx_(ctx)
           ,acceptor_(ioc) // 1. 先只绑定 io_context，不立即 open/bind
           ,doc_root_(doc_root)
    {
        // 分步操作更安全
        beast::error_code ec;

        // 2. 显式打开协议类型
        tcp::endpoint endpoint(tcp::v4(), port_num);

        // 打开 Socket 监听器
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor open failed");
        }

        // 3. 在 bind 之前设置端口复用 (此时有效)
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw boost::system::system_error(ec, "set_option reuse_address failed");
        }

        // 4. 显式绑定端口
        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor bind failed");
        }

        // 5. 启动监听
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw boost::system::system_error(ec, "acceptor listen failed");
        }
    }                  

    void run(){
        do_accept();
    }
    
private:
    void do_accept(){
        // 待接入对象池
        // std::cout << "start to accept... " << "\n";
        auto& ioc = IOServicePool::GetInstance()->GetIOService();
        acceptor_.async_accept(ioc,beast::bind_front_handler(
            &Server::on_accept,this->shared_from_this()
        ));
    }

    void on_accept(beast::error_code ec,tcp::socket socket){
        if(ec){
            return;
        }

        if constexpr(is_ssl_stream_v<StreamType>){
            asio::ssl::stream<beast::tcp_stream> stream(std::move(socket),ctx_);
            std::make_shared<https_session>(std::move(stream),doc_root_)->run();
        }else{
            beast::tcp_stream stream(std::move(socket));
            std::make_shared<http_session>(std::move(stream),doc_root_)->run();
        }
        // std::cout << "finished accepting... " << "\n";
        do_accept();
    }
};