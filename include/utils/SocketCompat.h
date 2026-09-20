#pragma once

// 套接字跨线程安全工具：SIGPIPE 抑制与连接代际（generation）标识。
//
// 两件事在这里集中处理：
//  1) SIGPIPE —— 对端 RST / 半关闭后 write() 会向进程投递 SIGPIPE，默认动作是终止
//     进程。框架内部一律用 send(..., MSG_NOSIGNAL)，并在进程早期兜底忽略 SIGPIPE，
//     使"客户端提前断开"退化为可处理的 EPIPE/ECONNRESET。
//  2) 连接代际 —— 关闭一个连接后，fd 号会被内核立刻复用给新连接；在途的 worker
//     任务仍持有旧 fd 号。用 (fd, generation) 代替裸 fd 做归属校验，可确保"陈旧
//     fd 的完成回调"不会关闭已属于新连接的 fd。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>

namespace http {
namespace net {

// 连接标识：高 32 位为代际，低 32 位为 fd。
using ConnId = uint64_t;

inline ConnId makeConnId(int fd, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) |
           static_cast<uint32_t>(fd);
}

inline uint32_t connIdGeneration(ConnId id) {
    return static_cast<uint32_t>(id >> 32);
}

inline int connIdFd(ConnId id) {
    return static_cast<int>(id & 0xFFFFFFFFu);
}

// 每个连接分配一个各不相同且非 0 的代际号（全进程单调递增）。
inline uint32_t nextConnectionGeneration() {
    static std::atomic<uint32_t> counter{1};
    uint32_t g = counter.fetch_add(1, std::memory_order_relaxed);
    if (g == 0) {  // 回绕（需要 40 亿次连接）后重新起算，仍保证非 0
        g = counter.fetch_add(1, std::memory_order_relaxed);
    }
    return g;
}

// 忽略 SIGPIPE（幂等）。在任何写操作之前调用。
// 返回值：调用后 SIGPIPE 的 disposition 是否为 SIG_IGN。
bool ensureSigpipeIgnored();
bool isSigpipeIgnored();

// 带 MSG_NOSIGNAL 的 send 包装。
// 返回 recv/send 语义的 ssize_t；调用方仍需按 EAGAIN/EWOULDBLOCK/EINTR 分派。
ssize_t sendNoSignal(int fd, const void* buf, size_t len);

// 该次发送失败是否属于"连接已不可用"（不可重试，应关闭连接）。
inline bool isFatalSendError(int err) {
    return err != EAGAIN && err != EWOULDBLOCK && err != EINTR;
}

}  // namespace net
}  // namespace http
