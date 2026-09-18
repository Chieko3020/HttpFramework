// WebSocket 协议实现（RFC 6455）
// 移植自 WebsocketServer/src/ws/WebSocketCodec.cpp

#include "HttpFramework/wss/WebSocketCodec.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace http {
namespace wss {

namespace {

const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// RFC 3629 UTF-8 校验（拒绝过长编码、代理区、>U+10FFFF）（L9）
bool isValidUtf8(const uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        std::size_t extra;
        uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0u) == 0xC0u) { extra = 1; cp = c & 0x1Fu; if (cp == 0) return false; }
        else if ((c & 0xF0u) == 0xE0u) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8u) == 0xF0u) { extra = 3; cp = c & 0x07u; }
        else return false;
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((p[i + k] & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        // 过长编码 / 代理区 / 超出 Unicode 范围
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

std::string base64Encode(const unsigned char* input, int len) {
    std::string out;
    // EVP_EncodeBlock 会写 outLen+1 字节（末尾 NUL），缓冲必须多留 1 字节（L9）
    out.resize(static_cast<std::size_t>(4 * ((len + 2) / 3) + 1));
    int outLen = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), input, len);
    out.resize(static_cast<std::size_t>(outLen));
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool findHeaderValue(const std::string& headerBlock, const std::string& headerName,
                     std::string* out) {
    std::istringstream iss(headerBlock);
    std::string line;
    std::string target = toLower(headerName);
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string name = toLower(line.substr(0, pos));
        if (name == target) {
            std::string value = line.substr(pos + 1);
            while (!value.empty() && value[0] == ' ') value.erase(value.begin());
            *out = value;
            return true;
        }
    }
    return false;
}

}  // namespace

// ── 静态方法实现 ────────────────────────────────────────────────────

std::size_t WebSocketStreamParser::maxPayloadLimit() {
    static std::size_t cached = 0;
    if (cached != 0) return cached;
    cached = 64 * 1024;
    const char* v = std::getenv("HTTPFW_WSS_MAX_PAYLOAD_BYTES");
    if (!v) return cached;
    try {
        std::size_t parsed = static_cast<std::size_t>(std::stoul(v));
        if (parsed >= 1024) cached = parsed;
    } catch (...) {}
    return cached;
}

std::size_t WebSocketStreamParser::maxMessageLimit() {
    static std::size_t cached = 0;
    if (cached != 0) return cached;
    cached = maxPayloadLimit();   // 默认与单帧上限一致
    const char* v = std::getenv("HTTPFW_WSS_MAX_MESSAGE_BYTES");
    if (!v) return cached;
    try {
        std::size_t parsed = static_cast<std::size_t>(std::stoul(v));
        if (parsed >= 1024) cached = parsed;
    } catch (...) {}
    return cached;
}

std::size_t WebSocketStreamParser::maxUpgradeHeaderLimit() {
    static std::size_t cached = 0;
    if (cached != 0) return cached;
    cached = 16 * 1024;
    const char* v = std::getenv("HTTPFW_WSS_MAX_UPGRADE_BYTES");
    if (!v) return cached;
    try {
        std::size_t parsed = static_cast<std::size_t>(std::stoul(v));
        if (parsed >= 1024) cached = parsed;
    } catch (...) {}
    return cached;
}

bool WebSocketStreamParser::acceptNonce(const std::string& nonce) {
    static std::mutex mutex;
    static std::unordered_map<std::string, uint64_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const uint64_t ttl = 300;
    for (auto it = seen.begin(); it != seen.end();) {
        if (now - it->second > ttl)
            it = seen.erase(it);
        else
            ++it;
    }
    if (nonce.empty()) return false;
    if (seen.count(nonce)) return false;
    seen[nonce] = now;
    return true;
}

// ── WebSocketCodec ───────────────────────────────────────────────────

std::string WebSocketCodec::computeAccept(const std::string& secWebSocketKey) {
    std::string concat = secWebSocketKey + kGuid;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), hash);
    return base64Encode(hash, SHA_DIGEST_LENGTH);
}

std::vector<uint8_t> WebSocketCodec::buildFrame(uint8_t opcode,
                                                 const std::vector<uint8_t>& payload,
                                                 bool fin) {
    std::vector<uint8_t> out;
    out.reserve(2 + payload.size() + 8);

    uint8_t b0 = static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    out.push_back(b0);

    uint64_t len = payload.size();
    if (len <= 125) {
        out.push_back(static_cast<uint8_t>(len & 0x7F));
    } else if (len <= 0xFFFFu) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>(len & 0xFFu));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFFu));
    }

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> WebSocketCodec::buildTextFrame(const std::string& text) {
    return buildFrame(0x1, std::vector<uint8_t>(text.begin(), text.end()), true);
}

std::vector<uint8_t> WebSocketCodec::buildBinaryFrame(const std::vector<uint8_t>& data) {
    return buildFrame(0x2, data, true);
}

std::vector<uint8_t> WebSocketCodec::buildPing(const std::vector<uint8_t>& payload) {
    return buildFrame(0x9, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildPong(const std::vector<uint8_t>& payload) {
    return buildFrame(0xA, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildClose(uint16_t statusCode) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((statusCode >> 8) & 0xFFu));
    payload.push_back(static_cast<uint8_t>(statusCode & 0xFFu));
    return buildFrame(0x8, payload, true);
}

std::vector<uint8_t> WebSocketCodec::buildClientFrame(uint8_t opcode,
                                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(2 + payload.size() + 16);

    uint8_t b0 = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
    out.push_back(b0);

    uint64_t len = payload.size();
    if (len <= 125) {
        out.push_back(static_cast<uint8_t>(0x80 | (len & 0x7Fu)));
    } else if (len <= 0xFFFFu) {
        out.push_back(static_cast<uint8_t>(0x80 | 126));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>(len & 0xFFu));
    } else {
        out.push_back(static_cast<uint8_t>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFFu));
    }

    uint8_t mask_key[4];
    if (RAND_bytes(mask_key, 4) != 1) {
        std::cerr << "[ERROR][WebSocket]：RAND_bytes 生成掩码失败" << std::endl;
        return {};
    }
    for (int i = 0; i < 4; ++i) out.push_back(mask_key[i]);
    for (std::size_t i = 0; i < payload.size(); ++i)
        out.push_back(static_cast<uint8_t>(payload[i] ^ mask_key[i % 4]));

    return out;
}

std::vector<uint8_t> WebSocketCodec::buildClientTextFrame(const std::string& text) {
    return buildClientFrame(0x1, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> WebSocketCodec::buildClientBinaryFrame(const std::vector<uint8_t>& data) {
    return buildClientFrame(0x2, data);
}

std::vector<uint8_t> WebSocketCodec::buildClientPing(const std::vector<uint8_t>& payload) {
    return buildClientFrame(0x9, payload);
}

// ── WebSocketStreamParser ────────────────────────────────────────────

WebSocketStreamParser::WebSocketStreamParser() {}

WebSocketStreamParser::WebSocketStreamParser(bool requireMaskFromPeer) {
    require_mask_ = requireMaskFromPeer;
}

void WebSocketStreamParser::setOpenMode() {
    state_ = State::Open;
    parse_offset_ = 0;
    buffer_.clear();
    in_fragment_ = false;
    fragment_opcode_ = 0;
    fragment_payload_.clear();
}

bool WebSocketStreamParser::tryConsumeUpgrade(std::string* outAcceptResponse) {
    if (!outAcceptResponse) return false;

    static const std::string kCRLFCRLF = "\r\n\r\n";
    if (buffer_.size() < kCRLFCRLF.size()) return false;

    // 从上次扫到的位置续扫（回退 3 字节以覆盖跨批次的 CRLFCRLF），
    // 否则每次 feed 都从头重扫，大头部场景是 O(n²)（M18）
    std::size_t start = upgrade_scan_pos_ > 3 ? upgrade_scan_pos_ - 3 : 0;
    if (start + kCRLFCRLF.size() > buffer_.size()) start = 0;
    std::size_t endPos = std::string::npos;
    for (std::size_t i = start; i + kCRLFCRLF.size() <= buffer_.size(); ++i) {
        if (buffer_[i] == '\r' && buffer_[i + 1] == '\n' &&
            buffer_[i + 2] == '\r' && buffer_[i + 3] == '\n') {
            endPos = i + kCRLFCRLF.size();
            break;
        }
    }
    if (endPos == std::string::npos) {
        upgrade_scan_pos_ = buffer_.size();
        return false;
    }

    std::string headerBlock(reinterpret_cast<const char*>(buffer_.data()),
                            reinterpret_cast<const char*>(buffer_.data() + endPos));

    // 请求行必须是 GET（RFC 6455 §4.1）（L9）
    {
        auto lineEnd = headerBlock.find("\r\n");
        std::string requestLine = headerBlock.substr(0, lineEnd);
        if (requestLine.rfind("GET ", 0) != 0)
            throw WsProtocolError("WebSocket upgrade must use GET", 1002);
    }

    std::string secKey;
    if (!findHeaderValue(headerBlock, "Sec-WebSocket-Key", &secKey))
        throw WsProtocolError("Missing Sec-WebSocket-Key", 1002);
    if (secKey.empty()) throw WsProtocolError("Empty Sec-WebSocket-Key", 1002);

    std::string upgrade;
    if (!findHeaderValue(headerBlock, "Upgrade", &upgrade) || toLower(upgrade) != "websocket")
        throw WsProtocolError("Invalid Upgrade header", 1002);

    // Connection 头必须包含 upgrade（逐 token 判断，容忍 "keep-alive, Upgrade"）（L9）
    {
        std::string conn;
        if (!findHeaderValue(headerBlock, "Connection", &conn))
            throw WsProtocolError("Missing Connection header", 1002);
        conn = toLower(conn);
        bool hasUpgrade = false;
        std::size_t pos = 0;
        while (pos <= conn.size()) {
            auto comma = conn.find(',', pos);
            std::string token = conn.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                token.erase(token.begin());
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t' ||
                                      token.back() == '\r'))
                token.pop_back();
            if (token == "upgrade") { hasUpgrade = true; break; }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (!hasUpgrade)
            throw WsProtocolError("Connection header must contain upgrade", 1002);
    }

    // Sec-WebSocket-Version 必须是 13（L9）
    {
        std::string ver;
        if (!findHeaderValue(headerBlock, "Sec-WebSocket-Version", &ver))
            throw WsProtocolError("Missing Sec-WebSocket-Version", 1002);
        while (!ver.empty() && (ver.back() == ' ' || ver.back() == '\t' || ver.back() == '\r'))
            ver.pop_back();
        if (ver != "13")
            throw WsProtocolError("Unsupported Sec-WebSocket-Version", 1002);
    }

    if (enforce_nonce_) {
        std::string nonce;
        if (!findHeaderValue(headerBlock, "X-Nonce", &nonce))
            throw std::runtime_error("Missing X-Nonce in 0-RTT mode");
        if (!acceptNonce(nonce))
            throw std::runtime_error("Replay detected by X-Nonce");
    }

    std::string accept = WebSocketCodec::computeAccept(secKey);
    *outAcceptResponse =
        std::string("HTTP/1.1 101 Switching Protocols\r\n") +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(endPos));
    parse_offset_ = 0;
    state_ = State::Open;
    return true;
}

bool WebSocketStreamParser::feed(const uint8_t* data, std::size_t len,
                                  std::string* outAcceptResponse,
                                  std::vector<WsFrame>* outFrames) {
    if (!outAcceptResponse) return false;
    if (outFrames) outFrames->clear();

    outAcceptResponse->clear();
    if (len > 0) buffer_.insert(buffer_.end(), data, data + len);

    // 升级头长度上限：升级成功前不接受无界喂数据（M18）
    if (state_ == State::AwaitingHttpUpgrade &&
        buffer_.size() > maxUpgradeHeaderLimit()) {
        throw WsProtocolError("Upgrade header too large", 1002);
    }

    bool producedAccept = false;

    if (state_ == State::AwaitingHttpUpgrade) {
        try {
            producedAccept = tryConsumeUpgrade(outAcceptResponse);
        } catch (...) {
            throw;
        }
        if (state_ == State::AwaitingHttpUpgrade) return producedAccept;
    }

    const std::size_t kMaxPayload = maxPayloadLimit();
    const std::size_t kMaxMessage = maxMessageLimit();

    while (true) {
        if (buffer_.size() - parse_offset_ < 2) break;
        std::size_t idx = parse_offset_;

        uint8_t b0 = buffer_[idx];
        uint8_t b1 = buffer_[idx + 1];

        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool rsv = (b0 & 0x70) != 0;
        if (rsv) throw WsProtocolError("RSV bits set", 1002);

        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = static_cast<uint64_t>(b1 & 0x7F);
        idx += 2;

        if (opcode >= 0x8 && opcode <= 0xF) {
            if (!fin) throw WsProtocolError("Control frames must not be fragmented", 1002);
            if (payload_len > 125)
                throw WsProtocolError("Control frame payload too large", 1002);
        }

        if (payload_len == 126) {
            if (buffer_.size() - idx < 2) break;
            payload_len = (static_cast<uint64_t>(buffer_[idx]) << 8) |
                          static_cast<uint64_t>(buffer_[idx + 1]);
            idx += 2;
            // RFC 6455 §5.2：必须用最短编码（<=125 的值不得用 126 编码）（L9）
            if (payload_len <= 125)
                throw WsProtocolError("Non-minimal payload length encoding", 1002);
        } else if (payload_len == 127) {
            if (buffer_.size() - idx < 8) break;
            bool highBit = (buffer_[idx] & 0x80u) != 0;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | static_cast<uint64_t>(buffer_[idx + i]);
            idx += 8;
            // 最高位必须为 0（RFC 6455 §5.2），且同样要求最短编码
            if (highBit || payload_len <= 0xFFFFu)
                throw WsProtocolError("Non-minimal payload length encoding", 1002);
        }

        if (payload_len > kMaxPayload)
            throw WsProtocolError("WS payload too large", 1009);

        uint8_t mask_key[4] = {0, 0, 0, 0};
        if (masked) {
            if (buffer_.size() - idx < 4) break;
            std::memcpy(mask_key, &buffer_[idx], 4);
            idx += 4;
        } else {
            if (require_mask_)
                throw WsProtocolError("Masked bit not set on incoming frame", 1002);
        }

        if (buffer_.size() - idx < payload_len) break;

        std::vector<uint8_t> payload;
        payload.resize(static_cast<std::size_t>(payload_len));
        if (payload_len > 0)
            std::memcpy(payload.data(), &buffer_[idx], payload_len);

        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = payload[i] ^ mask_key[i % 4];
        }
        idx += static_cast<std::size_t>(payload_len);

        parse_offset_ = idx;

        // 分片消息处理
        if (opcode == 0x0) {
            if (!in_fragment_)
                throw WsProtocolError("Unexpected continuation frame", 1002);
            // 累计上限：每个分片各自合规还不够，总量必须有界（H12）
            if (fragment_payload_.size() + payload.size() > kMaxMessage)
                throw WsProtocolError("Fragmented message too large", 1009);
            fragment_payload_.insert(fragment_payload_.end(), payload.begin(), payload.end());
            if (fin) {
                if (fragment_opcode_ == 0x1 &&
                    !isValidUtf8(fragment_payload_.data(), fragment_payload_.size()))
                    throw WsProtocolError("Invalid UTF-8 in fragmented text message", 1007);
                WsFrame f;
                f.opcode = fragment_opcode_;
                f.fin = true;
                f.payload = std::move(fragment_payload_);
                in_fragment_ = false;
                fragment_opcode_ = 0;
                fragment_payload_.clear();
                if (outFrames) outFrames->push_back(std::move(f));
            }
            continue;
        }

        if (opcode == 0x1 || opcode == 0x2) {
            if (in_fragment_)
                throw WsProtocolError("New data frame while fragmented", 1002);
            if (fin) {
                if (opcode == 0x1 && !isValidUtf8(payload.data(), payload.size()))
                    throw WsProtocolError("Invalid UTF-8 in text frame", 1007);
                WsFrame f;
                f.opcode = opcode;
                f.fin = true;
                f.payload = std::move(payload);
                if (outFrames) outFrames->push_back(std::move(f));
            } else {
                if (payload.size() > kMaxMessage)
                    throw WsProtocolError("Fragmented message too large", 1009);
                in_fragment_ = true;
                fragment_opcode_ = opcode;
                fragment_payload_ = std::move(payload);
            }
            continue;
        }

        // 控制帧
        if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) {
            if (opcode == 0x8 && payload.size() == 1)
                throw WsProtocolError("Invalid close payload length", 1002);
            if (opcode == 0x8 && payload.size() > 2 &&
                !isValidUtf8(payload.data() + 2, payload.size() - 2))
                throw WsProtocolError("Invalid UTF-8 in close reason", 1007);
            if (opcode == 0x8 && payload.size() >= 2) {
                uint16_t code = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
                // RFC 6455 §7.4.1：非法/保留关闭码要以 1002 关闭（L9）
                if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 ||
                    code == 1006 || code == 1015 || (code > 1011 && code < 3000))
                    throw WsProtocolError("Invalid close code in close frame", 1002);
            }
            if (outFrames) {
                WsFrame f;
                f.opcode = opcode;
                f.fin = true;
                f.payload = std::move(payload);
                outFrames->push_back(std::move(f));
            }
            continue;
        }

        throw WsProtocolError("Unknown opcode", 1002);
    }

    if (parse_offset_ > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(parse_offset_));
        parse_offset_ = 0;
    }

    return producedAccept;
}

}  // namespace wss
}  // namespace http
