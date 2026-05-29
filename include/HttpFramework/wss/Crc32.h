#pragma once

// CRC32 校验类 — 移植自 WebsocketServer
// 用于文件分片校验和整文件完整性校验

#include <cstddef>
#include <cstdint>
#include <vector>

namespace http {
namespace wss {

class Crc32 {
public:
    Crc32();

    // 一次性计算
    uint32_t checksum(const uint8_t* data, std::size_t len) const;
    uint32_t checksum(const std::vector<uint8_t>& data) const;

    // 流式计算（大文件分块读取时使用）
    void reset();
    void update(const uint8_t* data, std::size_t len);
    uint32_t finish() const;

private:
    static uint32_t table_[256];
    static bool table_init_;
    static void initTable();
    uint32_t current_{0xFFFFFFFFu};
};

}  // namespace wss
}  // namespace http
