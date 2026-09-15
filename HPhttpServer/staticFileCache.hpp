#pragma once
// ============================================================================
//  staticFileCache.hpp —— 静态文件内存缓存 + 启动期预拼响应头
// ----------------------------------------------------------------------------
//  解决的问题（对应 httpSessionTemplate.hpp 里 handle_request 的 TODO）：
//
//    旧路径：每个请求都要
//      1. path_cat()  -> 构造 std::string（堆分配）
//      2. mime_type() -> 一串字符串比较
//      3. file_body::open() -> 内核 open()/fstat()，close()，中间还要 read()
//      4. 构造 response<file_body> + message_generator（堆分配 + 类型擦除）
//      5. beast 现场序列化响应头
//    实测：这些开销让静态文件吞吐比"内容已在内存 + 字节已拼好"低约 20%。
//
//    新路径：启动时把文件一次性读进内存，并把每个文件的响应头**预先拼好**；
//            请求到来时只做「一次哈希查找 + 一次 write」。
//
//  两个关键设计点，看代码时重点看这两处：
//    A. 异构查找（transparent_hash）：让 find(std::string_view) 不构造临时
//       std::string。否则每请求都要为查表分配一次内存，省下的开销又还回去了。
//    B. 头与体分开存：预拼的响应头单独放一个 std::string，body 单独放一个。
//       写的时候用两段 buffer 一次 writev 发出去，既不用把 body 拷贝进头里，
//       也不必每请求再拼一次 Content-Length。
//
//  取舍（都是有意为之，别当成 bug）：
//    * 内容常驻内存，且**不会热更新**：改了文件要重启（或以后自己加 reload()）。
//    * 单个文件超过 max_file_size、或总缓存量超过 max_total_bytes 的文件不入缓存，
//      这些路径会被记进 skipped_，请求时仍走旧的流式 path（不会变成 404）。
//    * 不做百分号解码：`/a%20b.html` 查不到，会落到 skipped/404。需要的话在这里加解码。
// ============================================================================

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <array>

namespace hp {

struct MimeEntry{
    std::string_view ext;
    std::string_view type;
};    

// 比大小写不敏感：std::string_view 没有 iequals，这里临时转小写比较太浪费（每请求一次），
// （旧代码用 beast::iequals，属于每请求一次函数调用；这里改成常量比较，零成本）
// 保持按字典序升序排列，方便二分查找
constexpr std::array<MimeEntry,20> mime_table = 
{{
    { ".bmp",   "image/bmp" },
    { ".css",   "text/css" },
    { ".gif",   "image/gif" },
    { ".htm",   "text/html" },
    { ".html",  "text/html" },
    { ".ico",   "image/vnd.microsoft.icon" },
    { ".jpeg",  "image/jpeg" },
    { ".jpg",   "image/jpeg" },
    { ".js",    "application/javascript" },
    { ".json",  "application/json" },
    { ".png",   "image/png" },
    { ".svg",   "image/svg+xml" },
    { ".tif",   "image/tiff" },
    { ".tiff",  "image/tiff" },
    { ".txt",   "text/plain" },
    { ".wasm",  "application/wasm" },
    { ".webp",  "image/webp" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".xml",   "application/xml" }
}};

// 忽略大小写的string_view辅助比较函数
inline bool iequals(std::string_view lhs,std::string_view rhs) noexcept {
    // 字长不一致，直接返回false
    if(lhs.size() != rhs.size()) return false;

    // 逐字节比较，若有某个字节不一致，直接返回false
    for(size_t i = 0; i < lhs.size(); ++i){
        if(std::tolower(static_cast<unsigned char>(lhs[i])) 
        != std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    // 一致，返回true
    return true;
}

// ----------------------------------------------------------------------------
// 扩展名 → MIME 类型
//       保证全工程只有这一张表，避免两处维护、慢慢长歪。
// ----------------------------------------------------------------------------
inline std::string_view mime_type_of(std::string_view path) noexcept
{
    // 取最后一个 '.' 之后的部分作为扩展名
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }

    // 获取 . 后拓展名
    std::string_view raw_ext = path.substr(dot);
    if(raw_ext.size() > 16) {
        return "application/octet-stream";
    }

    // 栈内存分配获得拓展名的小写
    char ext_buf[16];
    for(size_t i = 0; i < raw_ext.size(); ++i){
        ext_buf[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(raw_ext[i])));
    }
    std::string_view ext{ext_buf,raw_ext.size()};

    // 二分查找
    auto it = std::lower_bound(
        mime_table.begin(), mime_table.end(), ext,
        [](const MimeEntry& entry, std::string_view val) {
            return entry.ext < val;
        }
    );

    if (it != mime_table.end() && it->ext == ext) {
        return it->type;
    }

    // 若确实没有，返回其他类型
    return "application/octet-stream";
}

// ============================================================================
//  staticFileCache
// ============================================================================
class staticFileCache
{
public:
    // 单个文件在内存中的全部形态
    struct Entry
    {
        std::string path;             // URL 路径，如 "/img/logo.png"（只为调试/日志方便）
        std::string body;             // 文件内容本体
        std::string head_keep_alive;  // 预拼好的完整响应头，长连接版本
        std::string head_close;       // 预拼好的完整响应头，Connection: close 版本
    };

    static constexpr std::size_t kDefaultMaxFileSize  = 8u * 1024u * 1024u;    // 单文件上限 8 MiB
    static constexpr std::size_t kDefaultMaxTotalSize = 256u * 1024u * 1024u;  // 总缓存上限 256 MiB

    explicit staticFileCache(std::string doc_root,
                             std::size_t max_file_size  = kDefaultMaxFileSize,
                             std::size_t max_total_size = kDefaultMaxTotalSize)
        : doc_root_(std::move(doc_root)),
          max_file_size_(max_file_size),
          max_total_size_(max_total_size)
    {}

    // ------------------------------------------------------------------
    // 启动期扫描 + 读盘。必须在起线程之前、单线程调用一次。
    // load() 返回之后本对象只读：find() 可以随便并发调用，不需要锁。
    // ------------------------------------------------------------------
    void load();

    // ------------------------------------------------------------------
    // 热路径：URL 路径 → Entry*
    // 没命中返回 nullptr。查找过程不分配内存（见 transparent_hash）。
    // ------------------------------------------------------------------
    [[nodiscard]] const Entry* find(std::string_view url_path) const noexcept
    {
        const auto it = index_.find(url_path);   // 异构查找，不构造 std::string
        if (it == index_.end()) {
            return nullptr;
        }
        return &files_[it->second];
    }

    // 文件确实存在、但没进缓存（太大 / 超出总预算）→ 调用方应退回旧的流式 path，
    // 而不是回 404，否则会出现"文件在磁盘上却报 404"的怪现象。
    [[nodiscard]] bool is_skipped(std::string_view url_path) const noexcept
    {
        return skipped_.find(url_path) != skipped_.end();
    }

    [[nodiscard]] const std::string& doc_root() const noexcept { return doc_root_; }
    [[nodiscard]] std::size_t file_count()    const noexcept { return files_.size(); }
    [[nodiscard]] std::size_t loaded_bytes()  const noexcept { return loaded_bytes_; }
    [[nodiscard]] std::size_t skipped_count() const noexcept { return skipped_.size(); }

private:
    // ------------------------------------------------------------------
    // A. 异构查找用的哈希器
    //    std::unordered_map<std::string, T>::find(std::string_view) 在没有
    //    is_transparent 时会先把 string_view 转成 std::string —— 也就是
    //    **每个请求一次堆分配**。加上 is_transparent 之后，哈希和相等比较
    //    都可以直接用 string_view 完成，一次分配都不需要。
    //    （C++20 起 unordered_map 支持这种异构查找，工程用的是 C++23）
    // ------------------------------------------------------------------
    struct transparent_hash
    {
        using is_transparent = void;   // 这一行就是"允许异构查找"的开关

        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view>{}(sv);
        }
        // 不需要再为 std::string 写一个重载：std::string 能隐式转成 string_view，
        // 插入时也会走到上面这个函数，保证"插入用哪个哈希、查找就用哪个哈希"。
    };

    // 把 URL 路径写进索引（同时登记若干别名，见 load() 里的目录别名处理）
    void add_alias(std::string url, std::size_t file_index);

    // 预拼两种响应头（keep-alive / close）
    void build_headers(Entry& e, std::string_view content_type) const;

    std::string doc_root_;
    std::size_t max_file_size_;
    std::size_t max_total_size_;

    // B. 用 deque 而不是 vector：deque 在 push_back 时**不会让已有元素的地址失效**，
    //    所以即使以后有人手贱在 load() 之后又塞文件，已经发出去的 Entry* 依然有效。
    //    find() 返回的就是指向这里元素的指针。
    std::deque<Entry> files_;

    // URL 路径 → files_ 的下标。用下标而不是 Entry*，是为了让一个文件可以有多个别名
    // （"/" 和 "/index.html" 都指向同一份内容，不重复占内存）。
    std::unordered_map<std::string, std::size_t, transparent_hash, std::equal_to<>> index_;

    // 存在于磁盘但没进缓存的路径（值 = 文件大小，仅用于日志）
    std::unordered_map<std::string, std::size_t, transparent_hash, std::equal_to<>> skipped_;

    std::size_t loaded_bytes_ = 0;
};

// ----------------------------------------------------------------------------
// 注册一个 URL → 文件下标 的映射；已存在则不覆盖（先注册的优先）
// ----------------------------------------------------------------------------
inline void staticFileCache::add_alias(std::string url, std::size_t file_index)
{
    index_.emplace(std::move(url), file_index);
}

// ----------------------------------------------------------------------------
// 预拼响应头：这是"把每请求的拼装搬到启动期"的核心一步。
//
// 拼出来的字节形如（\r\n 已按 HTTP 规范展开）：
//     HTTP/1.1 200 OK\r\n
//     Server: beast\r\n
//     Content-Type: text/html\r\n
//     [Connection: close\r\n]        <- 只出现在短连接版本里
//     Content-Length: 143\r\n
//     \r\n
//
// 数字 143 用 std::to_chars 写进栈上的 char[20]：
//   * std::to_chars 是标准库唯一"零堆分配、不受 locale 影响"的整数格式化函数；
//   * 对比 std::to_string / std::ostream << 都会分配内存、走 locale 虚调用。
// 头信息预留 128 字节，一次分配到位，之后 append 不会再扩容。
// ----------------------------------------------------------------------------
inline void staticFileCache::build_headers(Entry& e, std::string_view content_type) const
{
    char nbuf[20];   // uint64 最多 20 位十进制；文件大小用不到这么多，够用
    const char* const nend = std::to_chars(nbuf, nbuf + sizeof(nbuf),
                                          static_cast<std::uint64_t>(e.body.size())).ptr;

    // 两遍循环：第 0 遍拼长连接版，第 1 遍拼短连接版。
    // 两版只差一行 Connection: close，所以用同一个函数生成，避免两处写歪。
    for (int variant = 0; variant < 2; ++variant) {
        const bool keep_alive = (variant == 0);
        std::string& out = keep_alive ? e.head_keep_alive : e.head_close;

        out.reserve(128);
        out.append("HTTP/1.1 200 OK\r\nServer: beast\r\nContent-Type: ");
        out.append(content_type);

        if (!keep_alive) {
            out.append("\r\nConnection: close");
        }

        out.append("\r\nContent-Length: ");
        out.append(nbuf, static_cast<std::size_t>(nend - nbuf));  // 栈上的数字，直接搬字节
        out.append("\r\n\r\n");                                   // 空行 = 头结束，body 从下一字节开始
    }
}

// ----------------------------------------------------------------------------
// 启动期扫描 + 读盘
// ----------------------------------------------------------------------------
inline void staticFileCache::load()
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (doc_root_.empty() || !fs::is_directory(doc_root_, ec)) {
        std::cerr << "[staticFileCache] 根目录不可用: '" << doc_root_
                  << "' (" << ec.message() << ")，缓存为空，所有请求都会走旧的流式路径\n";
        return;
    }

    std::size_t skipped_by_size  = 0;
    std::size_t skipped_by_total = 0;

    // recursive_directory_iterator：递归遍历整个站点目录。
    //   * skip_permission_denied：没有权限的子目录直接跳过，不要在这里抛异常
    //   * 不跟随目录符号链接（默认行为），避免软链接成环导致死循环
    for (const auto& dir_entry :
         fs::recursive_directory_iterator(doc_root_,
                                          fs::directory_options::skip_permission_denied, ec))
    {
        if (ec) {
            break;
        }

        // 只要普通文件（soft link 指向普通文件也会被 is_regular_file 判为 true，
        // 这与旧代码 path_cat + open() 的行为一致）
        if (!dir_entry.is_regular_file(ec)) {
            continue;
        }

        const auto size = static_cast<std::size_t>(dir_entry.file_size(ec));

        // 相对路径 → URL 路径：统一分隔符为 '/'，前面补一个 '/'
        // generic_string() 在 Windows 上也会把 '\' 换成 '/'
        std::string url = "/" + fs::relative(dir_entry.path(), doc_root_, ec).generic_string();
        if (ec || url.empty()) {
            ec.clear();
            continue;
        }

        // 上限 1：单个文件太大，不进内存，记进 skipped_ 让它走旧的流式路径
        if (size > max_file_size_) {
            skipped_.emplace(url, size);
            ++skipped_by_size;
            continue;
        }
        // 上限 2：总缓存量封顶，防止把整个 home 目录塞进内存（doc_root 配错时的保险丝）
        if (loaded_bytes_ + size > max_total_size_) {
            skipped_.emplace(url, size);
            ++skipped_by_total;
            continue;
        }

        // ---- 读文件 ----
        std::ifstream in(dir_entry.path(), std::ios::binary);
        if (!in) {
            skipped_.emplace(url, size);
            continue;
        }

        Entry e;
        e.path = url;
        e.body.resize(size);                 // 已知大小：一次分配到位，避免边读边扩容
        in.read(e.body.data(), static_cast<std::streamsize>(size));
        // 读到的可能少于 stat 出来的大小（文件正好被改小），按实际读到的字节数收缩，
        // 否则 Content-Length 会比实际 body 长，客户端会一直等剩下的字节（→ 假死/超时）
        e.body.resize(static_cast<std::size_t>(in.gcount()));

        // ---- 预拼响应头（Content-Length 就是 e.body.size()）----
        build_headers(e, mime_type_of(url));

        loaded_bytes_ += e.body.size();

        const std::size_t idx = files_.size();
        files_.push_back(std::move(e));

        // 注册 URL → 下标
        add_alias(url, idx);

        // 目录别名：/sub/index.html 额外登记成 "/sub/" 和 "/sub"
        //（旧代码是靠 path_cat + 追加 "index.html" 在运行时做的，
        //  放到启动期做，热路径就不用再拼字符串了）
        constexpr std::string_view kIndex = "/index.html";
        if (url.size() >= kIndex.size() &&
            std::string_view(url).substr(url.size() - kIndex.size()) == kIndex)
        {
            std::string dir = url.substr(0, url.size() - kIndex.size());   // "/sub" 或 ""
            if (dir.empty()) {
                add_alias("/", idx);            // 站点根："/" == "/index.html"
            } else {
                add_alias(dir + "/", idx);      // "/sub/" 
                add_alias(dir, idx);            // "/sub"（不带斜杠也认）
            }
        }
    }

    std::cout << "[staticFileCache] 根目录 " << doc_root_ << "\n"
              << "  已缓存 " << files_.size() << " 个文件, "
              << loaded_bytes_ << " 字节（约 " << (loaded_bytes_ / 1024) << " KiB）\n"
              << "  未缓存(单文件超限) " << skipped_by_size
              << " 个, 未缓存(总预算超限) " << skipped_by_total
              << " 个 —— 这些路径仍走旧的流式路径"
              << std::endl;   // 这里用 endl 强制刷一行：stdout 重定向到文件时是块缓冲，
                              // 不刷的话进程被 kill 掉就看不到这条启动日志
}

} // namespace hp
