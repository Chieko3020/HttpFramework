// WSS 事件循环实现

#include "HttpFramework/wss/WssReactor.h"
#include "HttpFramework/wss/WebSocketCodec.h"
#include "HttpFramework/wss/OpenSslHelpers.h"
#include "HttpFramework/wss/WsRouter.h"
#include "HttpFramework/wss/FileTransferPlugin.h"
#include "utils/ThreadPool.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <unordered_set>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

// ── WssConnection 方法在 namespace http ──
namespace http {

WssConnection::WssConnection(int fd, void* ssl, uint64_t connId)
    : state(fd, static_cast<SSL*>(ssl), connId) {}

WssConnection::~WssConnection() = default;

uint64_t WssConnection::id() const { return state.id; }
bool WssConnection::isOpen() const {
    return state.tls_done && state.ws_upgraded && !state.closing && state.fd.load() >= 0;
}
std::string WssConnection::remoteAddr() const { return state.remoteAddr; }

void WssConnection::sendText(const std::string& text) {
    sendBinary(wss::WebSocketCodec::buildTextFrame(text));
}
void WssConnection::sendBinary(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(state.outbound_mu);
    if (state.closing || state.fd.load() < 0 || !state.ssl) return;
    WssOutboundItem item;
    item.data = std::make_shared<std::vector<uint8_t>>(data);
    item.offset = 0;
    state.outbound.push_back(std::move(item));
}
void WssConnection::sendPing(const std::vector<uint8_t>& payload) {
    sendBinary(wss::WebSocketCodec::buildPing(payload));
}
void WssConnection::close(uint16_t code) {
    std::lock_guard<std::mutex> lk(state.outbound_mu);
    if (state.closing) return;
    state.closing = true;
    state.closeCode = code;   // 供 onClose 上报（H11）
    state.close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto closeFrame = wss::WebSocketCodec::buildClose(code);
    WssOutboundItem item;
    item.data = std::make_shared<std::vector<uint8_t>>(std::move(closeFrame));
            item.offset = 0;
    state.outbound.push_back(std::move(item));
}
void WssConnection::setUserData(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(state.userDataMu);
    state.userData[key] = value;
}
std::string WssConnection::getUserData(const std::string& key) const {
    std::lock_guard<std::mutex> lk(state.userDataMu);
    auto it = state.userData.find(key);
    return it != state.userData.end() ? it->second : std::string();
}
bool WssConnection::hasUserData(const std::string& key) const {
    std::lock_guard<std::mutex> lk(state.userDataMu);
    return state.userData.count(key) > 0;
}

}  // namespace http

// ── 以下为 WSS 内部实现 ──
namespace http {
namespace wss {

// ══════════════════════════════════════════════════════════════════════
// 内部工具
// ══════════════════════════════════════════════════════════════════════

namespace {

// 关闭帧发出后允许的最长滞留时间：到期强制 close(fd)
constexpr auto kCloseGrace = std::chrono::milliseconds(1000);

// 升级请求前缀（只需请求行）的保留上限
constexpr std::size_t kPreUpgradePrefixBytes = 4096;

bool isValidCloseCode(uint16_t code) {
    if (code < 1000 || code >= 5000) return false;
    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) return false;
    return true;
}

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

uint32_t baseInterest() { return static_cast<uint32_t>(EPOLLET | EPOLLRDHUP); }

int createListenSocket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); throw std::runtime_error("bind() failed");
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd); throw std::runtime_error("listen() failed");
    }
    if (setNonBlocking(fd) < 0) {
        ::close(fd); throw std::runtime_error("setNonBlocking failed");
    }
    return fd;
}

void updateInterest(int epoll_fd, int fd, bool want_write) {
    if (fd < 0) return;
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = baseInterest() | EPOLLIN;
    if (want_write) ev.events |= EPOLLOUT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

void updatePeak(std::atomic<uint64_t>* peak, uint64_t value) {
    uint64_t oldv = peak->load(std::memory_order_relaxed);
    while (value > oldv &&
           !peak->compare_exchange_weak(oldv, value, std::memory_order_relaxed)) {}
}

void notifyIoThreadOutbound(int wake_fd) {
    if (wake_fd < 0) return;
    uint64_t one = 1;
    if (::write(wake_fd, &one, sizeof(one)) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[WARN][WSS]：eventfd唤醒失败, errno=" << errno << std::endl;
    }
}

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
const char* earlyDataStatusLabel(const SSL* ssl) {
    switch (SSL_get_early_data_status(ssl)) {
        case SSL_EARLY_DATA_ACCEPTED: return "ACCEPTED";
        case SSL_EARLY_DATA_REJECTED: return "REJECTED";
        case SSL_EARLY_DATA_NOT_SENT: return "NOT_SENT";
        default: return "UNKNOWN";
    }
}
#endif

// ── TLS 读缓冲池 ────────────────────────────────────────────────────

class TlsReadPool {
public:
    TlsReadPool(std::size_t blockSize, std::size_t preallocCount)
        : blockSize_(blockSize) {
        owned_.reserve(preallocCount);
        for (std::size_t i = 0; i < preallocCount; ++i) {
            auto* block = new uint8_t[blockSize];
            owned_.push_back(block);
            ownedSet_.insert(block);
            freeList_.push_back(block);
        }
    }
    ~TlsReadPool() { for (auto* p : owned_) delete[] p; }

    uint8_t* acquire() {
        std::lock_guard<std::mutex> lk(mu_);
        if (freeList_.empty()) {
            heapFallback_++;
            if (heapFallback_ % 64 == 1)
                std::cerr << "[WARN][WSS]：TLS读缓冲池耗尽, 堆回退#" << heapFallback_ << std::endl;
            return new uint8_t[blockSize_];
        }
        auto* p = freeList_.back();
        freeList_.pop_back();
        return p;
    }
    void release(uint8_t* p) {
        std::lock_guard<std::mutex> lk(mu_);
        // 用集合做 O(1) 归属判断：旧实现每次归还要线性扫 owned_（512 项）（L7）
        if (ownedSet_.count(p) > 0) {
            freeList_.push_back(p);
            return;
        }
        delete[] p;   // 池耗尽时的堆回退缓冲
    }
    std::size_t blockSize() const { return blockSize_; }

private:
    std::size_t blockSize_;
    std::vector<uint8_t*> owned_, freeList_;
    std::unordered_set<uint8_t*> ownedSet_;
    std::mutex mu_;
    std::atomic<uint64_t> heapFallback_{0};
};

class TlsReadGuard {
public:
    explicit TlsReadGuard(TlsReadPool* pool) : pool_(pool), buf_(pool_->acquire()) {}
    ~TlsReadGuard() { pool_->release(buf_); }
    uint8_t* data() { return buf_; }
    std::size_t size() const { return pool_->blockSize(); }
private:
    TlsReadPool* pool_;
    uint8_t* buf_;
};

// ── 前向声明 ────────────────────────────────────────────────────────

bool flushOutbound(WssReactorState* st, std::shared_ptr<WssConnection> c);

// ── 连接关闭 ────────────────────────────────────────────────────────

void closeConnection(WssReactorState* st, int fd) {
    auto it = st->conns.find(fd);
    if (it == st->conns.end()) return;
    auto c = it->second;
    {
        std::lock_guard<std::mutex> lk(c->state.outbound_mu);
        c->state.closing = true;
    }
    st->conns.erase(it);
    st->activeFds.erase(fd);
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, fd, &ev);

    // onClose 必须在 ::close 之前调用（H11）：
    // 此前全仓没有任何调用者，onOpen/onClose 不对称，用户侧资源回收永不执行。
    // 回调运行在 I/O 线程（见 WsRouter.h 的说明）。
    if (st->wsRouter && c->state.ws_upgraded) {
        st->wsRouter->onClose(c->state.upgradePath, *c, c->state.closeCode);
        c->state.ws_upgraded = false;   // 防重入：closeConnection 可能被重复调用
    }

    // M16：必须先结束 TLS 会话再 close(fd)。
    // 反过来的话，BIO 仍持有已关闭（且可能已被内核复用）的 fd 号，
    // 析构里的 SSL_shutdown 会把 close_notify 写进一个无关的 fd。
    if (c->state.ssl) {
        SSL_shutdown(c->state.ssl);   // 非阻塞套接字：发出 close_notify 后立即返回
        SSL_free(c->state.ssl);
        c->state.ssl = nullptr;
    }
    int closingFd = c->state.fd.load();
    if (closingFd >= 0) {
        c->state.fd.store(-1);        // 先置 -1，worker 侧 isOpen() 立刻为 false
        ::close(closingFd);
    }
    std::cout << "[INFO][WSS]：连接关闭, fd=" << fd << " id=" << c->id()
              << " code=" << c->state.closeCode << std::endl;
}

// ── 消息处理 ────────────────────────────────────────────────────────

bool processWsInboundBuffer(WssReactorState* st, int fd,
                             std::shared_ptr<WssConnection> c,
                             utils::ThreadPool& pool,
                             std::shared_ptr<WsRouter> router,
                             int wake_fd,
                             const uint8_t* buf, int ret,
                             bool* stop_read_burst) {
    *stop_read_burst = false;
    c->state.last_activity = std::chrono::steady_clock::now();

    std::string acceptResp;
    std::vector<WsFrame> frames;

    // 跨 TLS 记录累积升级数据，升级完成后提取请求路径。
    // 只需要请求行（最多 4KB），因此这里设上限：原先无界累积等于把升级前的
    // 每个字节都存了两份（解析器一份 + 这里一份）（M18）。
    if (!c->state.ws_upgraded && c->state.preUpgradeBuf.size() < kPreUpgradePrefixBytes) {
        std::size_t room = kPreUpgradePrefixBytes - c->state.preUpgradeBuf.size();
        std::size_t take = std::min(room, static_cast<std::size_t>(ret));
        c->state.preUpgradeBuf.insert(c->state.preUpgradeBuf.end(), buf, buf + take);
    }

    try {
        c->state.ws.feed(buf, static_cast<std::size_t>(ret), &acceptResp, &frames);
    } catch (const WsProtocolError& ex) {
        // RFC 6455：以协议错误对应的关闭码回一个 close 帧再断开
        // （1002 协议错误 / 1007 非法 UTF-8 / 1009 消息过大）
        std::cerr << "[ERROR][WSS]：协议错误, id=" << c->id() << ": " << ex.what()
                  << " (close=" << ex.closeCode << ")" << std::endl;
        c->state.closeCode = ex.closeCode;
        c->state.closing = true;
        c->state.close_deadline = std::chrono::steady_clock::now() + kCloseGrace;
        auto closeFrame = WebSocketCodec::buildClose(ex.closeCode);
        {
            std::lock_guard<std::mutex> lk(c->state.outbound_mu);
            WssOutboundItem item;
            item.data = std::make_shared<std::vector<uint8_t>>(std::move(closeFrame));
            item.offset = 0;
            c->state.outbound.push_back(std::move(item));
        }
        updateInterest(st->epoll_fd, c->state.fd, true);
        if (c->state.tls_done) flushOutbound(st, c);
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR][WSS]：帧解析失败, id=" << c->id() << ": " << ex.what() << std::endl;
        return false;
    }

    if (!acceptResp.empty()) {
        c->state.ws_upgraded = true;
        c->state.last_server_ping_sent = std::chrono::steady_clock::now();

        // 从累积的升级数据中提取请求路径
        if (!c->state.preUpgradeBuf.empty()) {
            std::string raw(reinterpret_cast<const char*>(c->state.preUpgradeBuf.data()),
                            std::min(c->state.preUpgradeBuf.size(), std::size_t(4096)));
            auto cr = raw.find("\r\n");
            if (cr != std::string::npos) {
                auto line = raw.substr(0, cr);
                auto p1 = line.find(' ');
                auto p2 = line.rfind(' ');
                if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1)
                    c->state.upgradePath = line.substr(p1 + 1, p2 - p1 - 1);
            }
            c->state.preUpgradeBuf.clear();
        }
        std::vector<uint8_t> respBytes(acceptResp.begin(), acceptResp.end());
        {
            std::lock_guard<std::mutex> lk(c->state.outbound_mu);
            WssOutboundItem item;
            item.data = std::make_shared<std::vector<uint8_t>>(std::move(respBytes));
            item.offset = 0;
            c->state.outbound.push_back(std::move(item));
            updatePeak(&st->tx_queue_peak, static_cast<uint64_t>(c->state.outbound.size()));
        }
        updateInterest(st->epoll_fd, fd, true);
        if (c->state.tls_done) flushOutbound(st, c);
        if (router) router->onOpen(c->state.upgradePath, *c);
    }

    for (const auto& frame : frames) {
        if (frame.opcode == 0x9) {
            c->state.last_ping = std::chrono::steady_clock::now();
            auto pong = WebSocketCodec::buildPong(frame.payload);
            {
                std::lock_guard<std::mutex> lk(c->state.outbound_mu);
                WssOutboundItem item;
                item.data = std::make_shared<std::vector<uint8_t>>(std::move(pong));
                item.offset = 0;
                c->state.outbound.push_back(std::move(item));
            }
            updateInterest(st->epoll_fd, fd, true);
            if (c->state.tls_done) flushOutbound(st, c);
            continue;
        }
        if (frame.opcode == 0xA) { c->state.last_ping = std::chrono::steady_clock::now(); continue; }
        if (frame.opcode == 0x8) {
            c->state.closing = true;
            c->state.close_deadline = std::chrono::steady_clock::now() + kCloseGrace;
            uint16_t code = 1000;
            if (frame.payload.size() >= 2) {
                code = static_cast<uint16_t>((frame.payload[0] << 8) | frame.payload[1]);
                // 客户端发来的非法码（解析器已在 L9 里拒绝大部分）→ 1002
                if (!isValidCloseCode(code)) code = 1002;
            }
            c->state.closeCode = code;   // 供 onClose 上报（H11）
            auto closeResp = WebSocketCodec::buildClose(code);
            {
                std::lock_guard<std::mutex> lk(c->state.outbound_mu);
                WssOutboundItem item;
                item.data = std::make_shared<std::vector<uint8_t>>(std::move(closeResp));
                item.offset = 0;
                c->state.outbound.push_back(std::move(item));
            }
            updateInterest(st->epoll_fd, fd, true);
            if (c->state.tls_done) flushOutbound(st, c);
            *stop_read_burst = true;
            return true;
        }
        if (frame.opcode == 0x1 || frame.opcode == 0x2) {
            c->state.last_ping = std::chrono::steady_clock::now();
            auto payloadCopy = frame.payload;
            auto opcode = frame.opcode;
            std::weak_ptr<WssConnection> weak = c;
            // 只捕获任务真正需要的东西：wake_fd 用于唤醒 I/O 线程（见
            // releaseResources 里"wake_fd 保留到析构"的说明），router 用于分发。
            // 原先还捕获了未使用的 epoll_fd / metricsSt 裸指针（M17）。
            pool.enqueue([weak, payloadCopy, opcode, wake_fd, router]() mutable {
                auto conn = weak.lock();
                if (!conn || !conn->isOpen()) return;
                WsMessage msg;
                msg.opcode = opcode;
                msg.payload = std::move(payloadCopy);
                if (router) router->dispatch(conn->state.upgradePath, *conn, msg);
                notifyIoThreadOutbound(wake_fd);
            });
        }
    }
    if (c->state.closing) *stop_read_burst = true;
    return true;
}

// ── TLS 握手 ────────────────────────────────────────────────────────

int driveTlsEarlyRead(WssReactorState* st, std::shared_ptr<WssConnection> c,
                       utils::ThreadPool& pool, std::shared_ptr<WsRouter> router,
                       int wake_fd, TlsReadPool* tlsPool) {
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    while (true) {
        TlsReadGuard bufGuard(tlsPool);
        uint8_t* buf = bufGuard.data();
        const int bufLen = static_cast<int>(bufGuard.size());
        size_t readbytes = 0;
        int ed = SSL_read_early_data(c->state.ssl, buf, static_cast<size_t>(bufLen), &readbytes);
        if (ed == SSL_READ_EARLY_DATA_SUCCESS) {
            // 0-RTT 复放的唯一判据是"这条连接确实接受了 early data"：
            // 只有这种情况才要求 X-Nonce（TLS 层的 anti-replay 仍是第一道防线，
            // 应用层 nonce 负责"同一 nonce 只用一次"的语义）（H9）
            c->state.ws.setEnforceNonce(
                SSL_get_early_data_status(c->state.ssl) == SSL_EARLY_DATA_ACCEPTED);
            if (readbytes > 0) {
                bool stop = false;
                if (!processWsInboundBuffer(st, c->state.fd, c, pool, router, wake_fd,
                                            buf, static_cast<int>(readbytes), &stop))
                    return -1;
                if (stop) return 1;
            }
            continue;
        }
        if (ed == SSL_READ_EARLY_DATA_FINISH) return 1;
        {
            int err = SSL_get_error(c->state.ssl, 0);
            if (err == SSL_ERROR_WANT_READ) { updateInterest(st->epoll_fd, c->state.fd, false); return 0; }
            if (err == SSL_ERROR_WANT_WRITE) { updateInterest(st->epoll_fd, c->state.fd, true); return 0; }
            std::cerr << "[ERROR][WSS-TLS]：SSL_read_early_data失败, id=" << c->id() << std::endl;
            return -1;
        }
    }
#else
    (void)st; (void)c; (void)pool; (void)router; (void)wake_fd; (void)tlsPool;
    return 1;
#endif
}

int driveTlsAccept(WssReactorState* st, std::shared_ptr<WssConnection> c) {
    while (true) {
        int ret = SSL_accept(c->state.ssl);
        if (ret == 1) {
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
            std::cout << "[INFO][WSS-TLS]：TLS握手完成, id=" << c->id()
                      << " early_data=" << earlyDataStatusLabel(c->state.ssl)
                      << " session_reused=" << (SSL_session_reused(c->state.ssl) ? "1" : "0") << std::endl;
#endif
            st->handshake_ok.fetch_add(1, std::memory_order_relaxed);
            if (SSL_session_reused(c->state.ssl))
                st->handshake_reused.fetch_add(1, std::memory_order_relaxed);
            else
                st->handshake_new.fetch_add(1, std::memory_order_relaxed);
            c->state.tls_done = true;
            // 握手完成即登记进"活跃集合"（心跳/冲刷只遍历它）
            if (!c->state.closing && c->state.fd.load() >= 0) st->activeFds.insert(c->state.fd.load());
            updateInterest(st->epoll_fd, c->state.fd, false);
            return 1;
        }
        int err = SSL_get_error(c->state.ssl, ret);
        if (err == SSL_ERROR_WANT_READ) { updateInterest(st->epoll_fd, c->state.fd, false); return 0; }
        if (err == SSL_ERROR_WANT_WRITE) { updateInterest(st->epoll_fd, c->state.fd, true); return 0; }
        std::cerr << "[ERROR][WSS-TLS]：TLS握手失败, id=" << c->id() << std::endl;
        st->handshake_fail.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
}

int advanceTlsHandshake(WssReactorState* st, std::shared_ptr<WssConnection> c,
                         utils::ThreadPool& pool, std::shared_ptr<WsRouter> router,
                         int wake_fd, TlsReadPool* tlsPool) {
    if (c->state.tls_done) return 1;
    if (!c->state.tls_early_read_finished) {
        int er = driveTlsEarlyRead(st, c, pool, router, wake_fd, tlsPool);
        if (er < 0) return -1;
        if (er == 0) return 0;
        c->state.tls_early_read_finished = true;
    }
    if (!c->state.tls_done) return driveTlsAccept(st, c);
    return 1;
}

// ── 出站刷新 ────────────────────────────────────────────────────────
//
// 并发契约（③）：
//   * 只有 I/O 线程调用本函数 —— 所有调用点都在 reactorLoop / 握手驱动 /
//     processWsInboundBuffer 里，这些函数都跑在唯一的 I/O 线程上。
//   * worker 线程只做两件事：sendBinary/sendPing/close 在锁内 push_back，
//     以及 notifyOutbound 写 wake_fd。队列是 deque：只有 I/O 线程 pop_front，
//     worker 只 push_back，因此"队首元素"没有被其它线程改写。
//   * 据此把 SSL_write 挪到锁外：锁内只做"取出队首载荷 / 推进 offset /
//     pop_front"，锁外做 SSL_write。
//
// 顺序保证：出站字节的先后 = 队列元素顺序。锁外期间新数据只能追加到队尾，
// 不会被插到队首之前；offset 只在锁内推进，也不会跳序。因此即便多线程高频
// sendBinary，帧顺序仍严格等于入队顺序（test_wss_hardening 的
// test_outbound_order_under_concurrency，用 4KB SO_SNDBUF 强制部分写路径）。
//
// 两个实现约束（踩过）：
//   1) 交给 SSL_write 的指针必须**在重试之间保持同一地址**：OpenSSL 在
//      SSL_ERROR_WANT_WRITE 之后要求"用同样的指针与长度重试"，换地址或换长度
//      会返回 SSL_R_BAD_WRITE_RETRY（error:0A00007F）。因此载荷放入
//      shared_ptr<vector>（地址稳定），并且重试时传 item->data() + offset。
//   2) 载荷不能是"栈上/本地拷贝"：队列元素可能在锁外被 pop_front() 释放
//      （close() 清空队列），指针随即悬空。shared_ptr 让正在发送的那段字节
//      独立于队列存活（本函数内的 sendItem 持有一份引用）。
//
// 代价：sendBinary 每段多一次堆分配 + 拷贝（原先队列元素内联 vector 也要拷贝
// 一次，差别只是多一层 shared_ptr 控制块）。换来的是 SSL_write 不再持
// outbound_mu —— 慢客户端（发送缓冲满、WANT_WRITE）不再阻塞其它线程入队。
bool flushOutbound(WssReactorState* st, std::shared_ptr<WssConnection> c) {
    auto& s = c->state;
    if (s.fd.load() < 0 || !s.ssl) return true;

    while (true) {
        // 锁内：确认队首、取载荷引用、推进 offset
        std::shared_ptr<std::vector<uint8_t>> payload;
        std::size_t offset = 0;
        bool queue_empty = false;
        {
            std::lock_guard<std::mutex> lk(s.outbound_mu);
            if (s.fd.load() < 0) return true;
            // 队首可能有已发一半的项：先跳过已完成的
            while (!s.outbound.empty() && !s.outbound.front().data) s.outbound.pop_front();
            while (!s.outbound.empty() &&
                   s.outbound.front().offset >= s.outbound.front().data->size())
                s.outbound.pop_front();
            if (s.outbound.empty()) {
                queue_empty = true;
            } else {
                WssOutboundItem& it = s.outbound.front();
                payload = it.data;          // 持有一份引用，元素被弹出也不会悬空
                offset = it.offset;
            }
        }

        if (queue_empty) break;
        if (!payload || offset >= payload->size()) continue;

        // ── 锁外 SSL_write：同一 payload->data()+offset、同一长度重试 ──
        char* const base = reinterpret_cast<char*>(payload->data());
        const std::size_t left = payload->size() - offset;
        int ret = SSL_write(s.ssl, base + offset, static_cast<int>(left));
        if (ret <= 0) {
            int err = SSL_get_error(s.ssl, ret);
            if (err == SSL_ERROR_WANT_WRITE) { updateInterest(st->epoll_fd, s.fd, true); return false; }
            if (err == SSL_ERROR_WANT_READ) { updateInterest(st->epoll_fd, s.fd, false); return false; }
            // 致命错误（非 WANT_READ/WANT_WRITE）：不能既不弹队列也不关连接，
            // 否则出站队列永远不再前进，连接静默挂死到空闲超时（M14）。
            std::cerr << "[ERROR][WSS]：SSL_write致命错误, 关闭连接, id=" << c->id() << std::endl;
            {
                std::lock_guard<std::mutex> lk(s.outbound_mu);
                s.closing = true;
                s.close_deadline = std::chrono::steady_clock::now() + kCloseGrace;
            }
            return false;
        }

        st->tx_bytes_total.fetch_add(static_cast<uint64_t>(ret), std::memory_order_relaxed);
        // 锁内：按已发出的字节数推进 offset（只有本线程推进队首）
        {
            std::lock_guard<std::mutex> lk(s.outbound_mu);
            if (!s.outbound.empty() && s.outbound.front().data) {
                WssOutboundItem& it = s.outbound.front();
                if (it.offset + static_cast<std::size_t>(ret) <= it.data->size())
                    it.offset += static_cast<std::size_t>(ret);
                else
                    it.offset = it.data->size();
                if (it.offset >= it.data->size()) s.outbound.pop_front();
            }
        }
        if (static_cast<std::size_t>(ret) < left) {
            // 只写出去一部分：等 EPOLLOUT 再续（重试仍用同一 payload 与 offset）
            updateInterest(st->epoll_fd, s.fd, true);
            return false;
        }
    }

    updateInterest(st->epoll_fd, s.fd, false);
    return true;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════
// WssReactor 实现
// ══════════════════════════════════════════════════════════════════════

WssReactor::WssReactor(uint16_t port, const std::string& certFile,
                       const std::string& keyFile, utils::ThreadPool& threadPool)
    : port_(port), certFile_(certFile), keyFile_(keyFile), threadPool_(threadPool) {}

void WssReactor::setWsRouter(std::shared_ptr<WsRouter> router) {
    wsRouter_ = std::move(router);
    st.wsRouter = wsRouter_.get();   // 断开路径要在 I/O 线程回调 onClose（H11）
}
void WssReactor::setFileTransferPlugin(std::shared_ptr<FileTransferPlugin> ftp) { ftPlugin_ = std::move(ftp); }
void WssReactor::setWsIdleTimeout(int s) { st.idleTimeoutSec = s > 0 ? s : 120; }
void WssReactor::setWsPingInterval(int s) { st.pingIntervalSec = s > 0 ? s : 40; }
void WssReactor::setTlsConfig(const TlsConfig& cfg) { tlsConfig_ = cfg; }
void WssReactor::notifyOutbound() { notifyIoThreadOutbound(st.wake_fd); }

bool WssReactor::start() {
    if (running_.load()) return false;
    try {
        if (!tlsConfig_.certFile.empty()) certFile_ = tlsConfig_.certFile;
        if (!tlsConfig_.keyFile.empty())  keyFile_  = tlsConfig_.keyFile;
        if (tlsConfig_.certFile.empty())  tlsConfig_.certFile = certFile_;
        if (tlsConfig_.keyFile.empty())   tlsConfig_.keyFile  = keyFile_;

        int idle = wsIdleTimeoutSec_ > 0 ? wsIdleTimeoutSec_ : 120;
        int pingIv = wsPingIntervalSec_ > 0 ? wsPingIntervalSec_ : std::max(5, idle / 3);
        if (pingIv >= idle) pingIv = std::max(5, idle / 2);
        st.idleTimeoutSec = idle;
        st.pingIntervalSec = pingIv;

        OPENSSL_init_ssl(0, nullptr);
        st.ctx = createServerContext(tlsConfig_);
        if (!st.ctx) throw std::runtime_error("createServerContext returned nullptr");
        st.listen_fd = createListenSocket(port_);
        st.epoll_fd = epoll_create1(0);
        if (st.epoll_fd < 0) throw std::runtime_error("epoll_create1 failed");

        epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.data.fd = st.listen_fd;
        ev.events = EPOLLIN | baseInterest();
        if (epoll_ctl(st.epoll_fd, EPOLL_CTL_ADD, st.listen_fd, &ev) < 0)
            throw std::runtime_error("epoll_ctl ADD listen_fd failed");

        st.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (st.timer_fd < 0) throw std::runtime_error("timerfd_create failed");
        itimerspec its; std::memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 1; its.it_interval.tv_sec = 1;
        timerfd_settime(st.timer_fd, 0, &its, nullptr);
        epoll_event tev; std::memset(&tev, 0, sizeof(tev));
        tev.data.fd = st.timer_fd; tev.events = EPOLLIN | EPOLLET;
        epoll_ctl(st.epoll_fd, EPOLL_CTL_ADD, st.timer_fd, &tev);

        st.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (st.wake_fd < 0) throw std::runtime_error("eventfd failed");
        epoll_event wev; std::memset(&wev, 0, sizeof(wev));
        wev.data.fd = st.wake_fd; wev.events = EPOLLIN | EPOLLET;
        epoll_ctl(st.epoll_fd, EPOLL_CTL_ADD, st.wake_fd, &wev);

        running_.store(true);
        reactorThread_ = std::thread(&WssReactor::reactorLoop, this);
        std::cout << "[INFO][WSS]：服务监听启动, port=" << port_ << " idle=" << st.idleTimeoutSec
                  << "s ping=" << st.pingIntervalSec << "s" << std::endl;
        return true;
    } catch (const std::exception& ex) {
        // M14：启动失败必须把已分配的资源全部回滚，否则坏证书/端口占用的每次重试
        // 都会泄漏一个 SSL_CTX 与若干 fd
        std::cerr << "[ERROR][WSS]：服务启动失败: " << ex.what()
                  << "（回滚已分配资源）" << std::endl;
        releaseResources();
        return false;
    }
}

// 释放 reactor 持有的 SSL_CTX 与全部 fd（幂等，start 失败与 stop 都走这里）
void WssReactor::releaseResources() {
    if (reactorThread_.joinable()) {
        running_.store(false);
        reactorThread_.join();
    }
    // 循环里未收尾的连接在此统一关闭（含 TLS 收尾与 onClose 回调）
    while (!st.conns.empty()) {
        wss::closeConnection(&st, st.conns.begin()->first);
    }
    if (st.ctx) { SSL_CTX_free(st.ctx); st.ctx = nullptr; }
    if (st.listen_fd >= 0) { ::close(st.listen_fd); st.listen_fd = -1; }
    if (st.epoll_fd >= 0) { ::close(st.epoll_fd); st.epoll_fd = -1; }
    if (st.timer_fd >= 0) { ::close(st.timer_fd); st.timer_fd = -1; }
    // wake_fd 故意保留到对象析构（M17）：在途 worker 任务可能仍持有这个 fd 号并写入，
    // 只要它一直打开，那一笔写就落在自己的 eventfd 上，而不会落到被复用的无关 fd。
    st.wake_fd_open = (st.wake_fd >= 0);
}

void WssReactor::stop() {
    running_.store(false);
    releaseResources();
}

WssReactor::~WssReactor() {
    // 基类析构里 stop() 已经跑过；这里只收尾 wake_fd（见 releaseResources 的说明）
    if (st.wake_fd >= 0) { ::close(st.wake_fd); st.wake_fd = -1; }
}

void WssReactor::reactorLoop() {
    std::vector<epoll_event> events(1024);
    TlsReadPool tlsPool(16 * 1024, 512);

    while (running_.load()) {
        int n = epoll_wait(st.epoll_fd, events.data(), static_cast<int>(events.size()), 1000);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t e = events[i].events;

            if (fd == st.timer_fd) {
                while (true) { uint64_t x; if (::read(st.timer_fd, &x, sizeof(x)) <= 0) break; }
                auto now = std::chrono::steady_clock::now();
                const auto pingIv = std::chrono::seconds(st.pingIntervalSec);

                // 合并成一次遍历（原来三次全表遍历：发心跳、flushOutbound、判空闲）。
                // 心跳与冲刷只针对活跃连接，关闭态连接单独收集并清理。
                std::vector<std::shared_ptr<WssConnection>> toFlush;
                std::vector<int> toClose;
                for (int cfd : st.activeFds) {
                    auto it = st.conns.find(cfd);
                    if (it == st.conns.end()) continue;
                    auto& c = it->second;
                    auto& s = c->state;
                    if (s.closing) {
                        bool drained;
                        {
                            std::lock_guard<std::mutex> lk(s.outbound_mu);
                            drained = s.outbound.empty();
                        }
                        if (drained || now > s.close_deadline) toClose.push_back(cfd);
                        continue;
                    }
                    if (!s.tls_done || !s.ws_upgraded) continue;
                    auto since = std::chrono::duration_cast<std::chrono::seconds>(
                        now - s.last_server_ping_sent);
                    if (since >= pingIv) {
                        auto pf = WebSocketCodec::buildPing({});
                        { std::lock_guard<std::mutex> lk(s.outbound_mu);
                          WssOutboundItem oi;
                          oi.data = std::make_shared<std::vector<uint8_t>>(std::move(pf));
                          oi.offset = 0;
                          s.outbound.push_back(std::move(oi)); }
                        s.last_server_ping_sent = now; s.last_ping = now;
                        updateInterest(st.epoll_fd, cfd, true);
                    }
                    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - s.last_ping);
                    if (diff.count() > st.idleTimeoutSec) { toClose.push_back(cfd); continue; }
                    toFlush.push_back(c);
                }
                for (auto& c : toFlush) flushOutbound(&st, c);
                for (int cfd : toClose) closeConnection(&st, cfd);

                // 文件传输会话周期清理
                if (ftPlugin_) {
                    uint64_t nowSec = uint64_t(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    ftPlugin_->cleanup(nowSec, 600, 1024);
                }

                if ((std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() % 5) == 0) {
                    auto qs = threadPool_.getQueueSize();
                    std::cout << "[DEBUG][WSS-指标]："
                              << " q=" << qs
                              << " tx=" << st.tx_bytes_total.load()
                              << " hs_ok=" << st.handshake_ok.load()
                              << " hs_fail=" << st.handshake_fail.load()
                              << " reused=" << st.handshake_reused.load()
                              << std::endl;
                }
                continue;
            }

            if (fd == st.wake_fd) {
                while (true) { uint64_t x; if (::read(st.wake_fd, &x, sizeof(x)) <= 0) break; }
                // 只处理有出站数据的连接：activeFds 已经排除了未握手/关闭中的连接。
                // 每个连接的 outbound_mu 在循环体内短持有、随即释放，不需要全局锁。
                std::vector<std::shared_ptr<WssConnection>> toFlush;
                for (int cfd : st.activeFds) {
                    auto it = st.conns.find(cfd);
                    if (it == st.conns.end()) continue;
                    auto& s = it->second->state;
                    if (s.closing) continue;
                    bool has_data;
                    {
                        std::lock_guard<std::mutex> olk(s.outbound_mu);
                        has_data = !s.outbound.empty();
                    }
                    if (has_data) toFlush.push_back(it->second);
                }
                for (auto& c : toFlush) flushOutbound(&st, c);
                continue;
            }

            if (fd == st.listen_fd) {
                while (true) {
                    sockaddr_in peer; socklen_t peer_len = sizeof(peer);
                    int cfd = ::accept(st.listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
                    if (cfd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; break; }
                    if (setNonBlocking(cfd) < 0) { ::close(cfd); continue; }
                    if (socketSendBuf_ > 0) {
                        // 测试用：收窄发送缓冲，让大帧必然出现"部分写"（见 setSocketSendBuffer）
                        int snd = socketSendBuf_;
                        ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
                    }
                    SSL* ssl = SSL_new(st.ctx);
                    if (!ssl) { ::close(cfd); continue; }
                    SSL_set_accept_state(ssl);
                    SSL_set_fd(ssl, cfd);
                    auto conn = std::make_shared<WssConnection>(cfd, ssl, st.nextConnId++);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
                    conn->state.tls_early_read_finished = (SSL_CTX_get_max_early_data(st.ctx) == 0);
#else
                    conn->state.tls_early_read_finished = true;
#endif
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    conn->state.remoteAddr = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
                    st.conns[cfd] = conn;
                    epoll_event cev; std::memset(&cev, 0, sizeof(cev));
                    cev.data.fd = cfd; cev.events = EPOLLIN | baseInterest();
                    if (epoll_ctl(st.epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        closeConnection(&st, cfd); continue;
                    }
                }
                continue;
            }

            auto it = st.conns.find(fd);
            if (it == st.conns.end()) continue;
            auto c = it->second;

            if (e & EPOLLERR) { closeConnection(&st, fd); continue; }
            if ((e & (EPOLLHUP | EPOLLRDHUP)) && !(e & EPOLLIN)) { closeConnection(&st, fd); continue; }

            if ((e & EPOLLIN) || ((e & EPOLLOUT) && !c->state.tls_done)) {
                if (!c->state.tls_done) {
                    int adv = advanceTlsHandshake(&st, c, threadPool_, wsRouter_, st.wake_fd, &tlsPool);
                    if (adv < 0) { closeConnection(&st, fd); continue; }
                    // 握手未完成时**不能**调用 flushOutbound：它在出站队列为空时
                    // 会把 EPOLLOUT 清掉（updateInterest(false)），ET 模式下不会自动
                    // 重新武装 —— SSL_accept 再也拿不到可写事件，握手永久停滞（H10）。
                    // 握手期间只允许握手驱动改兴趣位。
                    if (adv == 0) { continue; }
                    flushOutbound(&st, c);
                }
            }

            if (e & EPOLLIN && c->state.tls_done) {
                while (true) {
                    TlsReadGuard guard(&tlsPool);
                    uint8_t* buf = guard.data();
                    int ret = SSL_read(c->state.ssl, buf, static_cast<int>(guard.size()));
                    if (ret > 0) {
                        bool stop = false;
                        if (!processWsInboundBuffer(&st, fd, c, threadPool_, wsRouter_, st.wake_fd,
                                                     buf, ret, &stop)) { closeConnection(&st, fd); break; }
                        if (stop || c->state.closing) break;
                        continue;
                    }
                    if (ret == 0) { closeConnection(&st, fd); break; }
                    int err = SSL_get_error(c->state.ssl, ret);
                    if (err == SSL_ERROR_WANT_READ) break;
                    if (err == SSL_ERROR_WANT_WRITE) { updateInterest(st.epoll_fd, fd, true); break; }
                    closeConnection(&st, fd); break;
                }
            }

            // 连接已进入关闭态且关闭帧已排空：立即收尾，不要等下一次事件。
            // 修复前这里依赖"先武装 EPOLLOUT 再清掉"的时序，关闭帧发出后
            // 连接既不会被 EPOLLOUT 收尾（兴趣位已被 flushOutbound 清掉），
            // 也不会被空闲清理处理（closing 连接被跳过）→ 僵尸连接（H11）。
            if (c->state.closing) {
                bool drained;
                {
                    std::lock_guard<std::mutex> lk(c->state.outbound_mu);
                    drained = c->state.outbound.empty();
                }
                if (drained) { closeConnection(&st, fd); continue; }
            }

            if (e & EPOLLOUT) {
                if (flushOutbound(&st, c) && c->state.closing) closeConnection(&st, fd);
            }
        }
    }

    // 资源的释放统一由 releaseResources() 负责（幂等、start 失败与 stop 共用）。
    // 这里只负责退出循环 —— 在途 worker 仍可能写 wake_fd，它必须保持打开（M17）。
    std::cout << "[INFO][WSS]：reactor 循环结束" << std::endl;
}

}  // namespace wss
}  // namespace http
