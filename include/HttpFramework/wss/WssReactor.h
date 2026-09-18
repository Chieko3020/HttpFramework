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
#include <unordered_set>
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
    bool wake_fd_open{false};   // wake_fd 是否仍打开（保留到析构，见 WssReactor.cpp）

    std::unordered_map<int, std::shared_ptr<WssConnection>> conns;
    // 已完成 TLS 握手、尚未进入 closing 的连接 fd 集合。
    // 心跳与出站冲刷只遍历这个集合：升级中的连接（tls_done=false）与正在关闭
    // 的连接（closing=true）都不会被转发/心跳触及，原来每轮 timer/wake 都要
    // 遍历全部连接并逐个判断这两个标志（连接数上千时是 O(N) 无效遍历）。
    // 只在 I/O 线程读写（连接建立、握手完成、关闭路径都发生在该线程）。
    std::unordered_set<int> activeFds;
    uint64_t nextConnId{1};
    // 用于在断开时回调 onClose（非拥有指针，由 WssReactor 持有 shared_ptr）（H11）
    WsRouter* wsRouter{nullptr};

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
    // 测试用：把新连接的 SO_SNDBUF 收窄，强制 SSL_write 走"只写出一部分 →
    // 挂 EPOLLOUT 续发"的路径（默认 0 = 不动，按内核默认值）
    void setSocketSendBuffer(int bytes) { socketSendBuf_ = bytes; }

    // 从线程池回调，唤醒 IO 线程冲刷出站队列
    void notifyOutbound();

    // 内部状态（公开，供实现文件内的函数直接访问）
    WssReactorState st;

private:
    void reactorLoop();
    // 释放 SSL_CTX 与全部 fd、关闭剩余连接（幂等；start 失败与 stop 共用）
    void releaseResources();

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
    int socketSendBuf_ = 0;   // 0 = 不设置（内核默认）
};

}  // namespace wss
}  // namespace http
