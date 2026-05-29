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
struct WssOutboundItem {
    std::vector<uint8_t> data;
    std::size_t offset{0};
};

struct WssConnectionState {
    int fd{-1};
    SSL* ssl{nullptr};
    uint64_t id{0};
    bool tls_done{false};
    bool tls_early_read_finished{false};
    wss::WebSocketStreamParser ws;
    std::atomic<bool> closing{false};  // atomic: reactor thread 写入, TP worker 读取
    bool ws_upgraded{false};

    std::deque<WssOutboundItem> outbound;
    std::mutex outbound_mu;

    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point last_ping;
    std::chrono::steady_clock::time_point last_server_ping_sent;

    std::unordered_map<std::string, std::string> userData;
    std::string remoteAddr;
    std::string upgradePath;
    std::vector<uint8_t> preUpgradeBuf;   // 累积升级前的原始字节，用于跨 TLS 记录解析路径

    WssConnectionState(int cfd, SSL* s, uint64_t cid)
        : fd(cfd), ssl(s), id(cid),
          last_activity(std::chrono::steady_clock::now()),
          last_ping(std::chrono::steady_clock::now()),
          last_server_ping_sent(std::chrono::steady_clock::now()) {}

    ~WssConnectionState() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
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
