#pragma once

// WSS 连接对象
// ConnectionState 公开定义，供 WssReactor/WsRouter 直接访问

#include "HttpFramework/wss/WebSocketCodec.h"

#include <openssl/ssl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace http {

// 前向声明
namespace wss { class WssReactor; }

// 出站队列项
//
// data 用 shared_ptr 而非内联 vector：flushOutbound 在**不持 outbound_mu** 的
// 情况下把指针交给 SSL_write，而队列元素可能在锁外被 pop_front() 释放
// （close() 会清空队列）。shared_ptr 让"正在发送的那段字节"独立于队列存活，
// 指针不会悬空，同时地址在整个元素的发送过程中保持稳定 —— OpenSSL 要求
// SSL_ERROR_WANT_WRITE 之后用"同样的指针与长度"重试，否则直接报
// SSL_R_BAD_WRITE_RETRY（error:0A00007F，本机实测）。
struct WssOutboundItem {
    std::shared_ptr<std::vector<uint8_t>> data;
    std::size_t offset{0};
};

struct WssConnectionState {
    // fd 会被 I/O 线程写（accept、断开时置 -1）、被 worker 线程读（isOpen），
    // 因此必须是原子的（M15）。ssl 指针的生命周期由关闭顺序保证（M16）。
    std::atomic<int> fd{-1};
    SSL* ssl{nullptr};
    uint64_t id{0};
    bool tls_done{false};
    bool tls_early_read_finished{false};
    wss::WebSocketStreamParser ws;
    std::atomic<bool> closing{false};  // atomic: reactor thread 写入, TP worker 读取
    bool ws_upgraded{false};
    // 关闭码：正常关闭 1000、going away 1001、异常断开 1006（默认）、
    // 协议错误 1002、消息过大 1009…；由 onClose 回调上报（H11）
    uint16_t closeCode{1006};

    std::deque<WssOutboundItem> outbound;
    std::mutex outbound_mu;

    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point last_ping;
    std::chrono::steady_clock::time_point last_server_ping_sent;
    // 进入 closing 后允许等待关闭帧发出去的最晚时刻；到期强制关闭，
    // 避免"关闭帧已发出但连接一直挂着"的僵尸连接（H11/M14）
    std::chrono::steady_clock::time_point close_deadline;

    // userData 会被两类线程访问：WsRouter::onOpen/onClose（I/O 线程）与
    // WsRouter::dispatch（线程池 worker，写 ws_param_*），因此加锁（③ 顺带修）
    std::unordered_map<std::string, std::string> userData;
    mutable std::mutex userDataMu;
    std::string remoteAddr;
    std::string upgradePath;
    std::vector<uint8_t> preUpgradeBuf;   // 累积升级前的原始字节，用于跨 TLS 记录解析路径

    WssConnectionState(int cfd, SSL* s, uint64_t cid)
        : fd(cfd), ssl(s), id(cid),
          last_activity(std::chrono::steady_clock::now()),
          last_ping(std::chrono::steady_clock::now()),
          last_server_ping_sent(std::chrono::steady_clock::now()) {}

    // 兜底析构：正常路径下 closeConnection 已经做过 SSL_shutdown/SSL_free/::close
    // （见 M16），这里只处理"连接对象一直被 worker 持有、直到 reactor 销毁"的收尾。
    ~WssConnectionState() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        int f = fd.load();
        if (f >= 0) { ::close(f); fd.store(-1); }
    }
};

class WssConnection : public std::enable_shared_from_this<WssConnection> {
public:
    ~WssConnection();

    // ── 发送 ──
    void sendText(const std::string& text);
    void sendBinary(const std::vector<uint8_t>& data);
    void sendPing(const std::vector<uint8_t>& payload = {});
    void close(uint16_t code = 1000);

    // ── 查询 ──
    uint64_t id() const;
    bool isOpen() const;
    std::string remoteAddr() const;

    // ── 用户数据（per-connection key-value）──
    void setUserData(const std::string& key, const std::string& value);
    std::string getUserData(const std::string& key) const;
    bool hasUserData(const std::string& key) const;

    // 内部状态（公开，供 WssReactor/WsRouter 使用）
    WssConnectionState state;

    // 构造函数（内部使用，由 WssReactor 创建连接）
    explicit WssConnection(int fd, void* ssl, uint64_t connId);
};

}  // namespace http
