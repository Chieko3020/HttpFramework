// WSS 事件循环实现

#include "HttpFramework/wss/WssReactor.h"
#include "HttpFramework/wss/WebSocketCodec.h"
#include "HttpFramework/wss/OpenSslHelpers.h"
#include "HttpFramework/wss/WsRouter.h"
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
    return state.tls_done && state.ws_upgraded && !state.closing && state.fd >= 0;
}
std::string WssConnection::remoteAddr() const { return state.remoteAddr; }

void WssConnection::sendText(const std::string& text) {
    sendBinary(wss::WebSocketCodec::buildTextFrame(text));
}
void WssConnection::sendBinary(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(state.outbound_mu);
    if (state.closing || state.fd < 0 || !state.ssl) return;
    WssOutboundItem item; item.data = data; item.offset = 0;
    state.outbound.push_back(std::move(item));
}
void WssConnection::sendPing(const std::vector<uint8_t>& payload) {
    sendBinary(wss::WebSocketCodec::buildPing(payload));
}
void WssConnection::close(uint16_t code) {
    std::lock_guard<std::mutex> lk(state.outbound_mu);
    if (state.closing) return;
    state.closing = true;
    auto closeFrame = wss::WebSocketCodec::buildClose(code);
    WssOutboundItem item; item.data = std::move(closeFrame); item.offset = 0;
    state.outbound.push_back(std::move(item));
}
void WssConnection::setUserData(const std::string& key, const std::string& value) {
    state.userData[key] = value;
}
std::string WssConnection::getUserData(const std::string& key) const {
    auto it = state.userData.find(key);
    return it != state.userData.end() ? it->second : std::string();
}
bool WssConnection::hasUserData(const std::string& key) const {
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
        for (std::size_t i = 0; i < preallocCount; ++i) {
            auto* block = new uint8_t[blockSize];
            owned_.push_back(block);
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
        for (auto* owned : owned_)
            if (owned == p) { freeList_.push_back(p); return; }
        delete[] p;
    }
    std::size_t blockSize() const { return blockSize_; }

private:
    std::size_t blockSize_;
    std::vector<uint8_t*> owned_, freeList_;
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
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, fd, &ev);
    if (c->state.fd >= 0) { ::close(c->state.fd); c->state.fd = -1; }
    std::cout << "[INFO][WSS]：连接关闭, fd=" << fd << " id=" << c->id() << std::endl;
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

    // 跨 TLS 记录累积升级数据，升级完成后提取请求路径
    if (!c->state.ws_upgraded) {
        c->state.preUpgradeBuf.insert(c->state.preUpgradeBuf.end(), buf, buf + ret);
    }

    try {
        c->state.ws.feed(buf, static_cast<std::size_t>(ret), &acceptResp, &frames);
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
            item.data = std::move(respBytes);
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
                WssOutboundItem item; item.data = std::move(pong); item.offset = 0;
                c->state.outbound.push_back(std::move(item));
            }
            updateInterest(st->epoll_fd, fd, true);
            if (c->state.tls_done) flushOutbound(st, c);
            continue;
        }
        if (frame.opcode == 0xA) { c->state.last_ping = std::chrono::steady_clock::now(); continue; }
        if (frame.opcode == 0x8) {
            c->state.closing = true;
            uint16_t code = 1000;
            if (frame.payload.size() >= 2) {
                code = static_cast<uint16_t>((frame.payload[0] << 8) | frame.payload[1]);
                if (!isValidCloseCode(code)) code = 1002;
            }
            auto closeResp = WebSocketCodec::buildClose(code);
            {
                std::lock_guard<std::mutex> lk(c->state.outbound_mu);
                WssOutboundItem item; item.data = std::move(closeResp); item.offset = 0;
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
            pool.enqueue([weak, payloadCopy, opcode, wake_fd, router,
                          epoll_fd = st->epoll_fd, metricsSt = st]() mutable {
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

bool flushOutbound(WssReactorState* st, std::shared_ptr<WssConnection> c) {
    auto& s = c->state;
    if (s.fd < 0 || !s.ssl) return true;
    std::unique_lock<std::mutex> lk(s.outbound_mu);
    if (s.closing && s.outbound.empty()) { updateInterest(st->epoll_fd, s.fd, false); return true; }
    if (s.outbound.empty()) { updateInterest(st->epoll_fd, s.fd, false); return true; }
    while (!s.outbound.empty()) {
        WssOutboundItem& item = s.outbound.front();
        const uint8_t* ptr = item.data.data() + item.offset;
        std::size_t remaining = item.data.size() - item.offset;
        if (remaining == 0) { s.outbound.pop_front(); continue; }
        int ret = SSL_write(s.ssl, ptr, static_cast<int>(remaining));
        if (ret > 0) {
            st->tx_bytes_total.fetch_add(static_cast<uint64_t>(ret), std::memory_order_relaxed);
            item.offset += ret;
            if (item.offset >= item.data.size()) s.outbound.pop_front();
            continue;
        }
        int err = SSL_get_error(s.ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE) { updateInterest(st->epoll_fd, s.fd, true); return false; }
        if (err == SSL_ERROR_WANT_READ) { updateInterest(st->epoll_fd, s.fd, false); return false; }
        std::cerr << "[ERROR][WSS]：SSL_write致命错误, id=" << c->id() << std::endl;
        return false;
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

WssReactor::~WssReactor() { stop(); }

void WssReactor::setWsRouter(std::shared_ptr<WsRouter> router) { wsRouter_ = std::move(router); }
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
        std::cerr << "[ERROR][WSS]：服务启动失败: " << ex.what() << std::endl;
        return false;
    }
}

void WssReactor::stop() {
    running_.store(false);
    if (reactorThread_.joinable()) reactorThread_.join();
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

                for (auto& kv : st.conns) {
                    auto& c = kv.second;
                    auto& s = c->state;
                    if (!s.tls_done || !s.ws_upgraded || s.closing) continue;
                    auto since = std::chrono::duration_cast<std::chrono::seconds>(now - s.last_server_ping_sent);
                    if (since < pingIv) continue;
                    auto pf = WebSocketCodec::buildPing({});
                    { std::lock_guard<std::mutex> lk(s.outbound_mu);
                      WssOutboundItem oi; oi.data = std::move(pf); oi.offset = 0;
                      s.outbound.push_back(std::move(oi)); }
                    s.last_server_ping_sent = now; s.last_ping = now;
                    updateInterest(st.epoll_fd, kv.first, true);
                }
                for (auto& kv : st.conns) {
                    if (!kv.second->state.tls_done || kv.second->state.closing) continue;
                    flushOutbound(&st, kv.second);
                }
                std::vector<int> toClose;
                for (const auto& kv : st.conns) {
                    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - kv.second->state.last_ping);
                    if (diff.count() > st.idleTimeoutSec) toClose.push_back(kv.first);
                }
                for (int cfd : toClose) closeConnection(&st, cfd);

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
                for (auto& kv : st.conns) {
                    if (!kv.second->state.tls_done || kv.second->state.closing) continue;
                    flushOutbound(&st, kv.second);
                }
                continue;
            }

            if (fd == st.listen_fd) {
                while (true) {
                    sockaddr_in peer; socklen_t peer_len = sizeof(peer);
                    int cfd = ::accept(st.listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
                    if (cfd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; break; }
                    if (setNonBlocking(cfd) < 0) { ::close(cfd); continue; }
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
                    if (adv == 0) { if (e & EPOLLOUT) flushOutbound(&st, c); continue; }
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

            if (e & EPOLLOUT) {
                if (flushOutbound(&st, c) && c->state.closing) closeConnection(&st, fd);
            }
        }
    }

    if (st.ctx) { SSL_CTX_free(st.ctx); st.ctx = nullptr; }
    if (st.listen_fd >= 0) { ::close(st.listen_fd); st.listen_fd = -1; }
    if (st.epoll_fd >= 0) { ::close(st.epoll_fd); st.epoll_fd = -1; }
    if (st.timer_fd >= 0) { ::close(st.timer_fd); st.timer_fd = -1; }
    if (st.wake_fd >= 0) { ::close(st.wake_fd); st.wake_fd = -1; }
}

}  // namespace wss
}  // namespace http
