// CRC32 实现 — 移植自 WebsocketServer

#include "HttpFramework/wss/Crc32.h"

namespace http {
namespace wss {

uint32_t Crc32::table_[256];
bool Crc32::table_init_ = false;

void Crc32::initTable() {
    if (table_init_) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            if (c & 1) c = 0xEDB88320u ^ (c >> 1);
            else c >>= 1;
        }
        table_[i] = c;
    }
    table_init_ = true;
}

Crc32::Crc32() { initTable(); }

uint32_t Crc32::checksum(const uint8_t* data, std::size_t len) const {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
        crc = table_[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Crc32::checksum(const std::vector<uint8_t>& data) const {
    return checksum(data.data(), data.size());
}

void Crc32::reset() { initTable(); current_ = 0xFFFFFFFFu; }

void Crc32::update(const uint8_t* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i)
        current_ = table_[(current_ ^ data[i]) & 0xFFu] ^ (current_ >> 8);
}

uint32_t Crc32::finish() const { return current_ ^ 0xFFFFFFFFu; }

}  // namespace wss
}  // namespace http
