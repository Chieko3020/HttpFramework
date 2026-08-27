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

std::string base64Encode(const unsigned char* input, int len) {
    std::string out;
    out.resize(static_cast<std::size_t>(4 * ((len + 2) / 3)));
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

bool WebSocketStreamParser::shouldEnforceNonce() {
    const char* v = std::getenv("HTTPFW_WSS_ENABLE_0RTT");
    return v && std::string(v) == "1";
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

    std::size_t endPos = std::string::npos;
    for (std::size_t i = 0; i + kCRLFCRLF.size() <= buffer_.size(); ++i) {
        if (buffer_[i] == '\r' && buffer_[i + 1] == '\n' &&
            buffer_[i + 2] == '\r' && buffer_[i + 3] == '\n') {
            endPos = i + kCRLFCRLF.size();
            break;
        }
    }
    if (endPos == std::string::npos) return false;

    std::string headerBlock(reinterpret_cast<const char*>(buffer_.data()),
                            reinterpret_cast<const char*>(buffer_.data() + endPos));

    std::string secKey;
    if (!findHeaderValue(headerBlock, "Sec-WebSocket-Key", &secKey))
        throw std::runtime_error("Missing Sec-WebSocket-Key");
    if (secKey.empty()) throw std::runtime_error("Empty Sec-WebSocket-Key");

    std::string upgrade;
    if (!findHeaderValue(headerBlock, "Upgrade", &upgrade) || toLower(upgrade) != "websocket")
        throw std::runtime_error("Invalid Upgrade header");

    if (shouldEnforceNonce()) {
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

    while (true) {
        if (buffer_.size() - parse_offset_ < 2) break;
        std::size_t idx = parse_offset_;

        uint8_t b0 = buffer_[idx];
        uint8_t b1 = buffer_[idx + 1];

        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool rsv = (b0 & 0x70) != 0;
        if (rsv) throw std::runtime_error("RSV bits set");

        bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = static_cast<uint64_t>(b1 & 0x7F);
        idx += 2;

        if (opcode >= 0x8 && opcode <= 0xF) {
            if (!fin) throw std::runtime_error("Control frames must not be fragmented");
            if (payload_len > 125)
                throw std::runtime_error("Control frame payload too large");
        }

        if (payload_len == 126) {
            if (buffer_.size() - idx < 2) break;
            payload_len = (static_cast<uint64_t>(buffer_[idx]) << 8) |
                          static_cast<uint64_t>(buffer_[idx + 1]);
            idx += 2;
        } else if (payload_len == 127) {
            if (buffer_.size() - idx < 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | static_cast<uint64_t>(buffer_[idx + i]);
            idx += 8;
        }

        if (payload_len > kMaxPayload)
            throw std::runtime_error("WS payload too large");

        uint8_t mask_key[4] = {0, 0, 0, 0};
        if (masked) {
            if (buffer_.size() - idx < 4) break;
            std::memcpy(mask_key, &buffer_[idx], 4);
            idx += 4;
        } else {
            if (require_mask_)
                throw std::runtime_error("Masked bit not set on incoming frame");
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
            if (!in_fragment_) throw std::runtime_error("Unexpected continuation frame");
            fragment_payload_.insert(fragment_payload_.end(), payload.begin(), payload.end());
            if (fin) {
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
            if (in_fragment_) throw std::runtime_error("New data frame while fragmented");
            if (fin) {
                WsFrame f;
                f.opcode = opcode;
                f.fin = true;
                f.payload = std::move(payload);
                if (outFrames) outFrames->push_back(std::move(f));
            } else {
                in_fragment_ = true;
                fragment_opcode_ = opcode;
                fragment_payload_ = std::move(payload);
            }
            continue;
        }

        // 控制帧
        if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) {
            if (opcode == 0x8 && payload.size() == 1)
                throw std::runtime_error("Invalid close payload length");
            if (outFrames) {
                WsFrame f;
                f.opcode = opcode;
                f.fin = true;
                f.payload = std::move(payload);
                outFrames->push_back(std::move(f));
            }
            continue;
        }

        throw std::runtime_error("Unknown opcode");
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
