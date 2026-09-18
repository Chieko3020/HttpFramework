#include "utils/SocketCompat.h"

#include <csignal>
#include <errno.h>

namespace http {
namespace net {

bool ensureSigpipeIgnored() {
    // sigaction 而非 signal()：语义确定，且不依赖实现的 SA_RESTART 选择。
    // 只在首次调用时安装（幂等），避免反复覆盖调用方可能设置的处理器。
    static std::atomic<bool> installed{false};
    if (installed.load(std::memory_order_acquire)) {
        return isSigpipeIgnored();
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGPIPE, &sa, nullptr) != 0) {
        return false;
    }
    installed.store(true, std::memory_order_release);
    return true;
}

bool isSigpipeIgnored() {
    struct sigaction cur;
    std::memset(&cur, 0, sizeof(cur));
    if (sigaction(SIGPIPE, nullptr, &cur) != 0) {
        return false;
    }
    return cur.sa_handler == SIG_IGN;
}

ssize_t sendNoSignal(int fd, const void* buf, size_t len) {
    return ::send(fd, buf, len, MSG_NOSIGNAL);
}

}  // namespace net
}  // namespace http
