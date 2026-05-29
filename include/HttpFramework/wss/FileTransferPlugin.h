#pragma once

// 文件分片传输插件 — 作为 WSS 中间件使用
// 拦截 binary 帧处理 FILE_START/QUERY/CHUNK 协议
// text 帧透传给用户 handler

#include "WssTypes.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace http {
namespace wss {

// 文件传输会话状态
struct FileTransferState {
    uint64_t file_id{0}, file_size{0};
    uint32_t chunk_size{0}, total_chunks{0}, expected_crc32{0};
    std::vector<uint8_t> bitmap;
    uint32_t received_count{0};
    int fd{-1};
    std::string temp_path, final_display_name;
    uint64_t create_ts{0}, last_update_ts{0};
};

class FileTransferPlugin {
public:
    explicit FileTransferPlugin(const std::string& uploadDir = "uploads");

    // WsMiddleware 接口：binary 帧拦截，text 帧透传
    void operator()(http::WssConnection& conn, WsMessage& msg,
                    std::function<void()> next);

    // 周期清理过期会话（由 WssReactor 定时调用）
    void cleanup(uint64_t nowSeconds, uint64_t ttlSeconds = 600,
                 std::size_t maxSessions = 1024);

    std::unordered_map<uint64_t, FileTransferState> states;
    std::string uploadDir;
    std::mutex mu_;
};

}  // namespace wss
}  // namespace http
