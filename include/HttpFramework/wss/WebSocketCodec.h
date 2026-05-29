#pragma once

// WebSocket 协议工具与增量解析器（RFC 6455）
// 移植自 WebsocketServer，命名空间改为 http::wss

#include <cstdint>
#include <string>
#include <vector>

namespace http {
namespace wss {

struct WsFrame {
    uint8_t opcode{0};  // 1=text, 2=binary, 8=close, 9=ping, 10=pong, 0=continuation
    bool fin{true};
    std::vector<uint8_t> payload;
};

// WebSocket 编码辅助类（RFC 6455）
class WebSocketCodec {
public:
    // 根据客户端 Sec-WebSocket-Key 计算服务端 Sec-WebSocket-Accept
    static std::string computeAccept(const std::string& secWebSocketKey);

    // 构建服务端发给客户端的帧（不加 mask）
    static std::vector<uint8_t> buildFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
                                           bool fin = true);
    static std::vector<uint8_t> buildTextFrame(const std::string& text);
    static std::vector<uint8_t> buildBinaryFrame(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> buildPing(const std::vector<uint8_t>& payload);
    static std::vector<uint8_t> buildPong(const std::vector<uint8_t>& payload);
    static std::vector<uint8_t> buildClose(uint16_t statusCode);

    // 构建客户端发给服务端的帧（RFC 要求必须加 mask）
    static std::vector<uint8_t> buildClientFrame(uint8_t opcode, const std::vector<uint8_t>& payload);
    static std::vector<uint8_t> buildClientTextFrame(const std::string& text);
    static std::vector<uint8_t> buildClientBinaryFrame(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> buildClientPing(const std::vector<uint8_t>& payload);
};

// WebSocket 流式增量解析器
class WebSocketStreamParser {
public:
    enum class State { AwaitingHttpUpgrade, Open };

    WebSocketStreamParser();
    explicit WebSocketStreamParser(bool requireMaskFromPeer);

    void setOpenMode();
    State state() const { return state_; }

    // 尝试消费 HTTP 升级请求；成功时生成完整 101 响应
    bool tryConsumeUpgrade(std::string* outAcceptResponse);

    // 输入新字节并解析；返回是否产生升级响应
    bool feed(const uint8_t* data, std::size_t len,
              std::string* outAcceptResponse, std::vector<WsFrame>* outFrames);

    // 服务端是否要求 0-RTT nonce
    static bool shouldEnforceNonce();
    static bool acceptNonce(const std::string& nonce);
    static std::size_t maxPayloadLimit();

private:
    State state_{State::AwaitingHttpUpgrade};
    std::vector<uint8_t> buffer_;
    std::size_t parse_offset_{0};
    bool require_mask_{true};
    bool in_fragment_{false};
    uint8_t fragment_opcode_{0};
    std::vector<uint8_t> fragment_payload_;
};

}  // namespace wss
}  // namespace http
