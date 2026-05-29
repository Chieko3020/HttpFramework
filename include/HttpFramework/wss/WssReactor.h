#pragma once

// WSS 事件循环 — 独立的 epoll 线程
// 处理 TLS 1.3 握手、WebSocket 帧 I/O、心跳超时

#include "WssTypes.h"
#include "WssConnection.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/ssl.h>

namespace utils {
class ThreadPool;
}

namespace http {

namespace wss {

class WsRouter;
class FileTransferPlugin;

// 内部运行时状态（公开，供实现文件内的函数直接访问）
struct WssReactorState {
    SSL_CTX* ctx{nullptr};
    int listen_fd{-1};
    int epoll_fd{-1};
    int timer_fd{-1};
    int wake_fd{-1};

    std::unordered_map<int, std::shared_ptr<WssConnection>> conns;
    uint64_t nextConnId{1};

    // 指标
    std::atomic<uint64_t> tx_queue_peak{0};
    std::atomic<uint64_t> tx_bytes_total{0};
    std::atomic<uint64_t> handshake_ok{0};
    std::atomic<uint64_t> handshake_fail{0};
    std::atomic<uint64_t> handshake_reused{0};
    std::atomic<uint64_t> handshake_new{0};

    int idleTimeoutSec{120};
    int pingIntervalSec{40};
};

class WssReactor {
public:
    WssReactor(uint16_t port, const std::string& certFile, const std::string& keyFile,
               utils::ThreadPool& threadPool);
    ~WssReactor();

    WssReactor(const WssReactor&) = delete;
    WssReactor& operator=(const WssReactor&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void setWsRouter(std::shared_ptr<WsRouter> router);
    void setFileTransferPlugin(std::shared_ptr<FileTransferPlugin> ftp);
    void setWsIdleTimeout(int seconds);
    void setWsPingInterval(int seconds);
    void setTlsConfig(const TlsConfig& cfg);

    // 从线程池回调，唤醒 IO 线程冲刷出站队列
    void notifyOutbound();

    // 内部状态（公开，供实现文件内的函数直接访问）
    WssReactorState st;

private:
    void reactorLoop();

    uint16_t port_;
    std::string certFile_;
    std::string keyFile_;
    TlsConfig tlsConfig_;
    utils::ThreadPool& threadPool_;

    std::shared_ptr<WsRouter> wsRouter_;
    std::shared_ptr<FileTransferPlugin> ftPlugin_;
    std::atomic<bool> running_{false};
    std::thread reactorThread_;

    int wsIdleTimeoutSec_ = 120;
    int wsPingIntervalSec_ = 0;
};

}  // namespace wss
}  // namespace http
