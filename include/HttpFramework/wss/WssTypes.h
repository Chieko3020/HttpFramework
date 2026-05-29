#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace http {

// 前向声明
class WssConnection;

namespace wss {

// ── WebSocket 消息 ──────────────────────────────────────────────────

struct WsMessage {
    uint8_t opcode{0};  // 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong
    std::vector<uint8_t> payload;

    bool isText() const { return opcode == 0x1; }
    bool isBinary() const { return opcode == 0x2; }
    bool isClose() const { return opcode == 0x8; }
    bool isPing() const { return opcode == 0x9; }
    bool isPong() const { return opcode == 0xA; }
    std::string text() const { return {payload.begin(), payload.end()}; }
};

// ── 回调类型 ────────────────────────────────────────────────────────

/// 消息处理器：收到匹配路径的消息时调用
using WsHandler = std::function<void(http::WssConnection& conn, const WsMessage& msg)>;

/// WS 中间件：拦截或变换消息，调用 next() 继续链
using WsMiddleware = std::function<void(http::WssConnection& conn, WsMessage& msg,
                                        std::function<void()> next)>;

/// 连接建立回调（WebSocket 升级完成后）
using WsOpenHandler = std::function<void(http::WssConnection& conn)>;

/// 连接关闭回调
using WsCloseHandler = std::function<void(http::WssConnection& conn, uint16_t code)>;

// ── TLS 配置 ────────────────────────────────────────────────────────

struct TlsConfig {
    std::string certFile;
    std::string keyFile;
    int minTlsVersion = 13;          // 12 = TLS 1.2-1.3, 13 = TLS 1.3 only
    bool enableSessionTicket = true;
    long sessionTimeoutSeconds = 300;
    bool enable0Rtt = false;
    std::size_t maxEarlyData = 16 * 1024;
};

}  // namespace wss
}  // namespace http
