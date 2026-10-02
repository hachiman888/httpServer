# HPhttpServer

> [中文](README.md) | English

An HTTP/1.1 server written from scratch in **C++20 + Boost.Asio / Beast**, containing two implementation routes (a hand-written session / a templated session) and one WebSocket implementation (ws + wss).

> **Scope**: this is a **learning project with no real business requirements**. So what it demonstrates is not "how to put a server into production", but rather:
> how to draw the layers, how to serve static files fast, and **how to measure performance numbers so you don't fool yourself**.
> If this is your first time in the repo, get it running first (section 1), then read the two routes in section 2, and finally do not skip the "measurement pitfalls" in section 4.

---

## 1. Running it in five minutes

### Dependencies

| Dependency | Requirement |
|---|---|
| Compiler | **GCC 13+** |
| Boost | ≥ 1.83 (asio / beast / system), ≤ 1.87 |
| OpenSSL | needed by the wss / HTTPS branches. This repo **ships no certificates** |
| CMake | ≥ 3.16 (only `main` goes through CMake) |
| jsoncpp | only the `main` branch needs it (POST `/email` parses JSON) |

```bash
# Ubuntu / Debian
sudo apt install g++-13 cmake ninja-build libboost-all-dev libssl-dev libjsoncpp-dev
```

### The three entry points

The repo contains three separate executables, each demonstrating one route. Note that `main` and `test2` **both listen on 8080 by default** — don't start them at the same time:

| Entry point | Default port | What it demonstrates | How it's built |
|---|---|---|---|
| `main` | 8080 | hand-written HTTP session + object pool + logic thread pool | CMake |
| `test2` | 8080 | templated HTTP/HTTPS + in-memory static file cache | manual g++ |
| `test3` | 8089 | WebSocket / WSS bidirectional communication (echo) | manual g++ |

```bash
# ---------- main ----------
cmake -S HPhttpServer -B build -G Ninja
cmake --build build -j
./build/main

# ---------- test2 (the template branch is not in CMakeLists, build it by hand) ----------
g++ -std=c++20 -O3 -DNDEBUG -I HPhttpServer -o HPhttpServer/test2 \
    HPhttpServer/test2.cpp HPhttpServer/IOServicePool.cpp \
    -lboost_system -lpthread -lssl -lcrypto
./HPhttpServer/test2

# ---------- test3 ----------
g++ -std=c++20 -O3 -DNDEBUG -I HPhttpServer -o HPhttpServer/test3 \
    HPhttpServer/test3.cpp HPhttpServer/IOServicePool.cpp \
    -lboost_system -lpthread -lssl -lcrypto
./HPhttpServer/test3
```

### Verifying it works

```bash
# main
curl -i http://127.0.0.1:8080/count          # 200, request counter page
curl -i http://127.0.0.1:8080/time           # 200, current timestamp page
curl -i http://127.0.0.1:8080/nope           # 404

# POST goes down the "logic thread pool" path (IO threads only do send/receive)
curl -i -X POST http://127.0.0.1:8080/email \
     -H 'Content-Type: application/json' -d '{"email":"a@b.c"}'

# test2: its document root is "the working directory at startup",
#       so start it from the directory you want to serve
mkdir -p /tmp/site && printf '<h1>hi</h1>\n' > /tmp/site/index.html
cd /tmp/site && /absolute/path/HPhttpServer/test2
curl -i http://127.0.0.1:8080/index.html     # 200, served from the in-memory cache
curl -i http://127.0.0.1:8080/               # 200, directory index -> index.html

# test3: WebSocket echo, whatever you send comes back
websocat ws://127.0.0.1:8089
```

### HTTPS / WSS (you currently have to supply your own certificate)

`ssl::context` needs a certificate loaded before the TLS handshake can complete, and there are no certificate files in the repo. Generate a self-signed one:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
        -days 365 -subj "/CN=localhost"
```

Then load the certificate into `ctx` in `test2.cpp` / `test3.cpp`, and switch the `Server<...>` template argument to the ssl variant:

```cpp
ctx.use_certificate_chain_file("cert.pem");
ctx.use_private_key_file("key.pem", ssl::context::pem);
// test2: Server<ssl::stream<beast::tcp_stream>>   test3: Server<websocket::stream<ssl::stream<beast::tcp_stream>>>
```

---

## 2. The three implementation routes

### Route A: `main` — hand-written session + raw byte replies

```
main ioc (accept)
   └─ for each incoming connection: take an httpSession from the object pool,
      bind it to one of the io_contexts in the ioc pool
        └─ async_read to parse the request
             ├─ GET  → assemble the reply bytes right on the IO thread (assemble()), async_write it out
             └─ POST → push into logicSystem's queue; a worker thread processes it and
                       posts the send back to that session's ioc
        └─ if keep-alive, go back to async_read; idle connections are closed
           after 60s by a steady_timer
```

Highlights:
- **`assemble()`** (`logicProcessLayer.cpp`): `std::to_chars` for the numbers plus plain `append` to build the HTTP message; the buffer is `clear()`ed so its capacity survives → zero allocation on the hot path.
- **Object pool** (`shared_object_pool.hpp`): raw memory blocks + placement new; on return it manually calls the destructor and throws the memory back onto the free queue.
- **Sharded lock** (`shardedSessionManager` in `httpServer.cpp`): sessions are hashed by uuid into different shards, each shard with its own `shared_mutex`.
- **The boundary between IO threads and logic threads**: heavy work (database, JSON) goes on the queue; IO threads only send and receive.

### Route B: the template branch — one codebase for both HTTP and HTTPS

`Session<Stream>` template + an `is_ssl_stream` partial specialization: `Stream = beast::tcp_stream` means HTTP, `Stream = ssl::stream<beast::tcp_stream>` means HTTPS, and the business code exists only once.

Static files go through an **in-memory cache with pre-built response headers** (`staticFileCache.hpp`):

1. **At startup**, recursively scan the document root and read the files into memory; at the same time use `std::to_chars` to **pre-build each file's complete response header** (one for keep-alive, one for `Connection: close`).
2. **On request**, all that's left is a single hash lookup. A `transparent_hash` (with `is_transparent`) lets `find(std::string_view)` query the table directly — otherwise every lookup would first construct a temporary `std::string`, giving back all the savings.
3. **On a hit**, it returns a `response_view`: two `asio::const_buffer`s pointing at the cached "response header" and "file contents", written out by `asio::async_write` in a single `sendmsg` (vectored write) — **no malloc, no body copy**.

Compared with the old approach (per-request `open`/`read`/`close` plus a fresh `std::string` that the header and body were fully copied into), this removes 3 syscalls and one full copy per request.

### Route C: WebSocket (`wsSessionTemplate.hpp` + `ws&wssServer.hpp`)

- One `asio::make_strand` per connection, so no hand-written locks are needed under multiple threads.
- `websocket::stream_base::timeout::suggested(role_type::server)` hands ping/pong heartbeats to beast.
- ws / wss are likewise split by `if constexpr`: TLS does `async_handshake` first, then `async_accept` completes the HTTP Upgrade.
- The current business logic is an **echo** (whatever is received is written straight back); real business logic belongs in `on_read` rather than writing back directly.

---

## 3. Code map

| File | Purpose |
|---|---|
| `main.cpp` | `main` entry: create the ioc / httpServer / signal handling, listen on 8080 |
| `httpServer.h/.cpp` | acceptor + `shardedSessionManager` (manages all sessions behind sharded read-write locks) |
| `httpSession.h/.cpp` | hand-written HTTP session: read request → route → assemble byte reply; idle timeout via `steady_timer` |
| `IOServicePool.h/.cpp` | N `io_context`s + one thread each, dispatched round-robin (default `hardware_concurrency()/2`) |
| `logicProcessLayer.h/.cpp` | route table, GET byte assembly, POST queue + worker thread pool |
| `Singleton.h` | singleton base class (`std::call_once`) |
| `shared_object_pool.hpp` | object pool (raw memory + placement new + custom deleter returning the block) |
| `httpSessionTemplate.hpp` | templated session: a static cache hit goes through `response_view`, a miss goes through beast's `handle_request` |
| `http_httpsServer.hpp` | `Server<StreamType>`: builds the static cache, accepts, hands the connection to a session |
| `staticFileCache.hpp` | in-memory static file cache + startup-time header pre-building + MIME table |
| `wsSessionTemplate.hpp` | WebSocket session template (shared by ws/wss) |
| `ws&wssServer.hpp` | WebSocket server template (acceptor + per-connection strand) |
| `test2.cpp` | entry point of the template HTTP branch |
| `test3.cpp` | entry point of the WebSocket branch |
| `sslServerRef.cpp` | reference code from Boost's official https example, **not part of the build** (it's in `.gitignore`) |

> The filename `ws&wssServer.hpp` contains an `&`, so remember to quote it when referring to it in a shell.

---

## 4. Performance and measurement (the section most worth reading)

### Order-of-magnitude reference

Same machine (i7-12700H, 20 logical cores = 6 P-cores + 8 E-cores) / WSL2, `wrk -c1000 -t4`, pinned cores + alternating A/B:

| Scenario | Observed throughput |
|---|---|
| `main`, ioc pool = cores/3 (6 ioc threads) | **0.75M – 1.0M** req/s |
| `main`, ioc pool = cores/2 (10 ioc threads) | ~0.46M req/s |
| template branch + static files, `open` per request | ~0.56M – 0.63M req/s |
| template branch + in-memory cache + two-segment buffer write | ~0.65M – 0.8M req/s |

This table only means anything with three caveats attached:

1. **Absolute values swing widely with host load; only alternating comparisons within the same time window are trustworthy.**
   Same `main` + cores/3: measured **0.84M – 1.0M** several times (once a full million), but only 0.75M during a period when the host was busy — identical code, identical parameters, identical machine; the only difference was how busy the host was while measuring (see pitfall 2 below).
2. **The repo currently defaults to cores/2.** To reproduce the million-request tier, change the divisor in `IOServicePool.h` back to `/3` and measure again.
3. **`main` differs by about 40% between `/3` and `/2`** (760k vs 460k, alternating within the same window, three rounds with no overlap) — so the ioc thread count is **a parameter to be measured, not something where more is better**.

### Three pitfalls that fooled me

1. **A `Socket errors: timeout N` from wrk is not necessarily a real timeout.**
   Latency samples beyond `-T` (2s by default) are **discarded and counted**, which is why `Latency Max 11ms` and `timeout 632` can appear together — those 632 samples never entered the histogram at all.
   This actually happened early in this repo: the root cause was **WSL's `CLOCK_REALTIME` jumping backwards by about 1.7 seconds**, while wrk times things with `gettimeofday()`, so an unsigned subtraction turned into an astronomical number and got recorded as a timeout.
   **How to tell**: check whether `requests in X.XXs` is shorter than `-d` — the shortfall is roughly equal to the clock jump.

2. **WSL2's `/proc/loadavg` reflects the Windows host's load**, not how busy things are inside this Linux VM. I've seen `loadavg 7.4` together with `%Cpu` 98.6% idle. When the host gets busy, your benchmark numbers drop across the board.

3. **Comparisons require pinned cores + alternating A/B + ≥10s per round + medians over several rounds.**
   `taskset -c 0-3 wrk ...`, with the server pinned to a different set of cores; run A/B alternately rather than "A first, then B". A single round, or two sets of numbers taken at different times, is basically worthless as evidence.

One more thing: `hardware_concurrency()` returns the **logical** core count, which is not the same as "the number of cores you can actually keep busy" (this machine's 20 threads = 6 P-cores + 8 E-cores), so using it directly as a parallelism ceiling is imprecise to begin with.

---

## 5. Known limitations (the honest list)

- **Protocol coverage**: HTTP/1.1 only; no HTTP/2, no Range/partial content, no `If-Modified-Since`/ETag, no gzip.
- **No percent-decoding of URLs**: `/a%20b.html` won't be found in the cache.
- **No dynamic routing and no database connection pool — see section 6 for why.**
- **The static cache is never hot-reloaded**: changing a file requires a restart; files larger than 8 MiB, or beyond a 256 MiB total budget, are not cached (they fall back to the streaming path rather than being wrongly reported as 404).
- **Many error branches are just `// 待报错` (TODO: report the error)**: no logging, no statistics, no graceful shutdown.
- **`test2` / `test3` are not in CMakeLists** and must be compiled by hand; the repo has no unit tests and no CI.
- **POST is implemented only in the `main` branch** (`/email`); the template branch rejects it outright with a 400 (`handle_request` only lets GET/HEAD through).
- **The templated session holds a `const staticFileCache&` member**: the cache must outlive every session. Right now `Server` owns it, which is safe; but the reference dangles if the cache is ever moved into a local scope.
- **No certificates**, so HTTPS/WSS requires generating your own and editing the code.
- **Throughput has never been specifically tuned** — it's at a "fast enough to make the point" level.

---

## 6. Why the project stops here

A server is **infrastructure**: its value can only be defined by the business running on top of it. This repo has learned what there was to learn — the asynchronous model, session management, object pools, zero-copy replies, protocol semantics (why WebSocket should not be used to ship static files), and how to judge performance by measurement rather than by feeling.

Adding more features from here (dynamic routing, more protocols, benchmark tuning) would need a real requirement to drive it, otherwise you are just writing code nobody uses. So as a **learning project** this is where it ends; if you want to build something on top of it, what's usually missing is the business layer, not the server layer.
