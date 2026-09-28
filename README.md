# HPhttpServer

用 **C++23 + Boost.Asio / Beast** 从零写起的 HTTP/1.1 服务器，包含两条实现路线（手写会话 / 模板化会话）和一个 WebSocket（ws + wss）实现。

> **定位**：这是个**练手项目，没有真实业务需求**。所以它演示的不是"怎么上线一个服务器"，而是：
> 分层怎么切、静态文件怎么发得快、以及**性能数字要怎么量才不至于骗自己**。
> 如果你是第一次看这个仓库，建议先跑起来（第 1 节），再看第 2 节的两条路线，最后一定要看第 4 节的"测量避坑"。

---

## 1. 五分钟跑起来

### 依赖

| 依赖 | 要求 |
|---|---|
| 编译器 | **GCC 14+** |
| Boost | ≥ 1.83（asio / beast / system） |
| OpenSSL | wss / HTTPS 分支需要。本仓库**不含证书** |
| CMake | ≥ 3.16（只有 `main` 走 CMake） |
| jsoncpp | 只有 `main` 分支需要（POST `/email` 解析 JSON） |

```bash
# Ubuntu / Debian
sudo apt install g++-14 cmake ninja-build libboost-all-dev libssl-dev libjsoncpp-dev
```

### 三个入口

仓库里有三个独立可执行文件，各演示一条路线。注意 `main` 和 `test2` **都默认监听 8080**，别同时启动：

| 入口 | 默认端口 | 演示内容 | 构建方式 |
|---|---|---|---|
| `main` | 8080 | 手写 HTTP 会话 + 对象池 + 逻辑线程池 | CMake |
| `test2` | 8080 | 模板化 HTTP/HTTPS + 静态文件内存缓存 | 手动 g++ |
| `test3` | 8089 | WebSocket / WSS 双向通信（echo） | 手动 g++ |

```bash
# ---------- main ----------
cmake -S HPhttpServer -B build -G Ninja
cmake --build build -j
./build/main

# ---------- test2（模板分支没进 CMakeLists，手动编）----------
g++-14 -std=c++23 -O3 -DNDEBUG -I HPhttpServer -o HPhttpServer/test2 \
    HPhttpServer/test2.cpp HPhttpServer/IOServicePool.cpp \
    -lboost_system -lpthread -lssl -lcrypto
./HPhttpServer/test2

# ---------- test3 ----------
g++-14 -std=c++23 -O3 -DNDEBUG -I HPhttpServer -o HPhttpServer/test3 \
    HPhttpServer/test3.cpp HPhttpServer/IOServicePool.cpp \
    -lboost_system -lpthread -lssl -lcrypto
./HPhttpServer/test3
```

### 验证一下

```bash
# main
curl -i http://127.0.0.1:8080/count          # 200，请求计数页
curl -i http://127.0.0.1:8080/time           # 200，当前时间戳页
curl -i http://127.0.0.1:8080/nope           # 404

# POST 会走"逻辑线程池"那条路（IO 线程只负责收发）
curl -i -X POST http://127.0.0.1:8080/email \
     -H 'Content-Type: application/json' -d '{"email":"a@b.c"}'

# test2：它的站点根目录是"启动时的工作目录"，所以要在你想服务的目录里启动它
mkdir -p /tmp/site && printf '<h1>hi</h1>\n' > /tmp/site/index.html
cd /tmp/site && /绝对路径/HPhttpServer/test2
curl -i http://127.0.0.1:8080/index.html     # 200，命中内存缓存
curl -i http://127.0.0.1:8080/               # 200，目录索引 -> index.html

# test3：WebSocket echo，发什么回什么
websocat ws://127.0.0.1:8089
```

### HTTPS / WSS（当前需要自己补证书）

`ssl::context` 要装了证书才能完成 TLS 握手，仓库里没有证书文件。生成自签证书：

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
        -days 365 -subj "/CN=localhost"
```

然后在 `test2.cpp` / `test3.cpp` 里给 `ctx` 加载证书，并把 `Server<...>` 的模板参数换成 ssl 版本：

```cpp
ctx.use_certificate_chain_file("cert.pem");
ctx.use_private_key_file("key.pem", ssl::context::pem);
// test2：Server<ssl::stream<beast::tcp_stream>>   test3：Server<websocket::stream<ssl::stream<beast::tcp_stream>>>
```

---

## 2. 三条实现路线

### 路线 A：`main` —— 手写会话 + 裸字节回复

```
主 ioc（accept）
   └─ 每来一个连接：从对象池取一个 httpSession，绑定到 ioc 池中的某个 io_context
        └─ async_read 解析请求
             ├─ GET  → 在 IO 线程里直接拼字节回复（assemble()），async_write 发出
             └─ POST → 丢进 logicSystem 的队列，worker 线程处理完再 post 回该 session 的 ioc 发送
        └─ 长连接则回到 async_read，空闲 60s 由 steady_timer 关掉
```

看点：
- **`assemble()`**（`logicProcessLayer.cpp`）：`std::to_chars` 转数字 + 纯 `append` 拼 HTTP 报文，缓冲区 `clear()` 保留 capacity → 热路径零分配。
- **对象池**（`shared_object_pool.hpp`）：裸内存块 + placement new，归还时手动调析构、内存扔回空闲队列。
- **分片锁**（`httpServer.cpp` 的 `shardedSessionManager`）：会话以 uuid 哈希分到不同分片，各分片一把 `shared_mutex`。
- **IO 线程与逻辑线程的边界**：重活（数据库、JSON）放队列，IO 线程只做收发。

### 路线 B：模板分支 —— 一套代码同时支持 HTTP 与 HTTPS

`Session<Stream>` 模板 + `is_ssl_stream` 偏特化：`Stream = beast::tcp_stream` 就是 HTTP，`Stream = ssl::stream<beast::tcp_stream>` 就是 HTTPS，业务代码只有一份。

静态文件走**内存缓存 + 预拼响应头**（`staticFileCache.hpp`）：

1. **启动期**递归扫描站点目录，把文件读进内存；同时用 `std::to_chars` 把每个文件的**完整响应头预先拼好**（长连接 / `Connection: close` 各一份）。
2. **请求到来**只做一次哈希查找。这里用了一个 `transparent_hash`（带 `is_transparent`）让 `find(std::string_view)` 直接查表——否则每次查表都会先构造一个临时 `std::string`，省下的开销又还回去了。
3. **命中后**返回 `response_view`：两个 `asio::const_buffer` 分别指向缓存里的"响应头"和"文件内容"，`asio::async_write` 用一次 `sendmsg`（多段聚集写）发出去——**不 malloc、不拷贝 body**。

对比旧写法（每请求 `open`/`read`/`close` + 新建 `std::string` 把头和体全量拷进去），省掉了每请求 3 个系统调用和一次全量拷贝。

### 路线 C：WebSocket（`wsSessionTemplate.hpp` + `ws&wssServer.hpp`）

- 每条连接一个 `asio::make_strand`，多线程下不需要手写锁。
- `websocket::stream_base::timeout::suggested(role_type::server)` 把 ping/pong 心跳交给 beast 管。
- ws / wss 同样用 `if constexpr` 分流：TLS 先 `async_handshake`，再 `async_accept` 完成 HTTP Upgrade。
- 当前业务逻辑是 **echo**（收到的消息原样写回），真实业务应该接在 `on_read` 里，而不是直接回写。

---

## 3. 代码地图

| 文件 | 作用 |
|---|---|
| `main.cpp` | `main` 入口：建 ioc / httpServer / 信号处理，监听 8080 |
| `httpServer.h/.cpp` | acceptor + `shardedSessionManager`（分片读写锁管理所有会话） |
| `httpSession.h/.cpp` | 手写 HTTP 会话：读请求 → 路由 → 拼字节回复；空闲超时用 `steady_timer` |
| `IOServicePool.h/.cpp` | N 个 `io_context` + 各自一个线程，轮询分派（默认 `hardware_concurrency()/2`） |
| `logicProcessLayer.h/.cpp` | 路由表、GET 的字节拼装、POST 队列 + worker 线程池 |
| `Singleton.h` | 单例基类（`std::call_once`） |
| `shared_object_pool.hpp` | 对象池（裸内存 + placement new + 自定义 deleter 归还） |
| `httpSessionTemplate.hpp` | 模板会话：静态缓存命中走 `response_view`，未命中走 beast 的 `handle_request` |
| `http_httpsServer.hpp` | `Server<StreamType>`：建静态缓存、accept、把连接交给 session |
| `staticFileCache.hpp` | 静态文件内存缓存 + 启动期预拼响应头 + MIME 表 |
| `wsSessionTemplate.hpp` | WebSocket 会话模板（ws/wss 共用） |
| `ws&wssServer.hpp` | WebSocket 服务器模板（acceptor + per-connection strand） |
| `test2.cpp` | 模板 HTTP 分支入口 |
| `test3.cpp` | WebSocket 分支入口 |
| `sslServerRef.cpp` | Boost 官方 https 例子参考代码，**不参与构建**（在 `.gitignore` 里） |

> 文件名 `ws&wssServer.hpp` 带 `&`，在 shell 里引用记得加引号。

---

## 4. 性能与测量（这一节最值得看）

### 量级参考

同一台机器（i7-12700H，20 逻辑核 = 6 性能核 + 8 能效核）/ WSL2，`wrk -c1000 -t4`，绑核 + 交替 A/B：

| 场景 | 观测吞吐 |
|---|---|
| `main`，ioc 池 = 核数/3（6 个 ioc 线程） | **0.75M – 1.0M** req/s |
| `main`，ioc 池 = 核数/2（10 个 ioc 线程） | ~0.46M req/s |
| 模板分支 + 静态文件，每请求 `open` 文件 | ~0.56M – 0.63M req/s |
| 模板分支 + 内存缓存 + 两段 buffer 写 | ~0.65M – 0.8M req/s |

看这张表必须带上三个前提：

1. **绝对值随宿主机负载大幅摆动，只有"同一时段内的交替对比"可信。**
   同样是 `main` + 核数/3：作者多次测到 **0.84M – 1.0M**（其中一次 100 万），我在宿主机较忙的时段只测到 0.75M —— 代码、参数、机器完全相同，差的只是"测的时候宿主机忙不忙"（原因见下面坑 2）。
2. **当前仓库默认是核数/2。** 想复现 100 万那一档，把 `IOServicePool.h` 里的除数改回 `/3` 再测。
3. **`main` 在 `/3` 与 `/2` 之间差了约 40%**（760k vs 460k，同一时段交替对比、三轮结果无重叠）——所以 ioc 线程数是**要测出来的参数，不是越大越好**。

### 三个把人骗过的坑

1. **wrk 报的 `Socket errors: timeout N` 不一定是真的超时。**
   `-T`（默认 2s）之外的延迟样本会被**丢弃并计数**，所以 `Latency Max 11ms` 和 `timeout 632` 能同时出现——那 632 个样本压根没进直方图。
   这个仓库里最初就碰上过：根因是 **WSL 的 `CLOCK_REALTIME` 往回跳了约 1.7 秒**，而 wrk 用 `gettimeofday()` 计时，无符号相减直接变成天文数字，就被记成 timeout。
   **判断方法**：看 `requests in X.XXs` 是不是比 `-d` 短——短的量基本等于时钟跳变量。

2. **WSL2 的 `/proc/loadavg` 反映的是 Windows 宿主机的负载**，不代表这个 Linux 里忙不忙。曾出现过 `loadavg 7.4` 而 `%Cpu` 98.6% idle 的情况。宿主机一忙，你的压测数字就整体下滑。

3. **对比必须绑核 + 交替 A/B + 每轮 ≥10s + 多轮取中位数。**
   `taskset -c 0-3 wrk ...`，服务端固定到另一组核；A/B 交替跑而不是"先跑 A 再跑 B"。单轮数字、或者拿不同时间的两组数字互比，结论基本不可信。

另外：`hardware_concurrency()` 给的是**逻辑核数**，不等于"能跑满的核数"（这台机器 20 线程 = 6 性能核 + 8 能效核），拿它直接当并行度上限本身就不精确。

---

## 5. 已知限制（诚实清单）

- **协议覆盖**：只支持 HTTP/1.1；没有 HTTP/2、没有 Range/断点续传、没有 `If-Modified-Since`/ETag、没有 gzip。
- **URL 不做百分号解码**：`/a%20b.html` 查不到缓存。
- **静态缓存不热更新**：改了文件必须重启；单文件 > 8 MiB 或总量 > 256 MiB 的文件不入缓存（会退回流式路径，不会误报 404）。
- **错误分支很多只写了 `// 待报错`**：没有日志、没有统计、没有优雅关闭。
- **`test2` / `test3` 不在 CMakeLists 里**，得手动 g++；整个仓库没有单元测试和 CI。
- **POST 只在 `main` 分支实现**（`/email`）；模板分支的 POST 仍走 beast 的原路径。
- **模板会话持有 `const staticFileCache&` 引用成员**：缓存的生命周期必须比所有 session 长。目前由 `Server` 持有，安全；但如果以后把缓存放到局部作用域就会悬垂。
- **没有证书**，HTTPS/WSS 要自己生成并改代码。
- **吞吐量没有专门调优过**，属于"够快、能说明问题"的水平。

---

## 6. 为什么这个项目停在这里

服务器是**基础设施**：它的价值只能由"跑在它上面的业务"来定义。这个仓库把该学的部分学完了——异步模型、会话管理、对象池、零拷贝回复、协议语义（为什么 WebSocket 不该用来发静态文件）、以及"怎么用测量而不是感觉判断性能"。

再往后加功能（动态路由、更多协议、压测调优）需要真实需求来驱动，否则只是在给自己加没人用的代码。所以它作为**学习项目**到此为止；如果你要基于它做东西，缺的通常是业务层，而不是服务器层。
