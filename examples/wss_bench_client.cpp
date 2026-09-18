// wss_bench_client.cpp — WSS 性能基准客户端
// 用法: wss_bench_client <host> <port> <path> [count] [msg_size] [rounds] [warmup]
// 默认: count=1000, msg_size=256, rounds=1, warmup=max(50, count/10)
// 示例: wss_bench_client localhost 9443 /echo 1000 256
//
// 单 TLS 连接、多消息发送，精确测量 WebSocket 吞吐量和延迟
//
// 统计口径：分位数用 nearest-rank 定义 idx = ceil(q*n)-1；中位数用标准定义
// （n 为偶数时取中间两元素平均）。n 越大分位数才有区分度，故默认 count=1000；
// 正式取数前先跑一轮预热（不计入统计）。
//
// ⚠️ 安全性说明（L10）：本客户端不校验证书（SSL_VERIFY_NONE），
// 只用于本机/内网基准测试，不是生产客户端范例。

#include "HttpFramework/wss/WebSocketCodec.h"
#include "HttpFramework/wss/OpenSslHelpers.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <numeric>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

// ── 工具函数 ──────────────────────────────────────────────

static int tcpConnect(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket() 失败: " << strerror(errno) << std::endl;
        return -1;
    }

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        std::cerr << "gethostbyname() 失败" << std::endl;
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect() 失败: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }
    return fd;
}

// ── 写入 ──────────────────────────────────────────────────

static bool writeAll(SSL* ssl, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        int n = SSL_write(ssl, p, static_cast<int>(remaining));
        if (n <= 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

// ── 统计口径 ──────────────────────────────────────────────
//
// nearest-rank 分位数：idx = ceil(q*n) - 1（q ∈ (0,1]）。
// 旧实现用 `size*0.99` 直接截断取下标，与 ceil 口径只在 0.99n 为整数（n 为
// 100 的倍数）时差一秩（少取一个样本）；但 n ≤ 100 时两种口径都会落到末元素
// （最大值）上——n 只有几十、上百条时 p99 本来就等于最大值，这是 nearest-rank
// 的固有性质，不是取数 bug。所以修分位定义之外还必须把样本量提上去：默认
// count=1000（旧脚本调用处是 10~100 条）。
static double percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    if (q <= 0.0) return sorted.front();
    if (q >= 1.0) return sorted.back();
    double rank = std::ceil(q * static_cast<double>(sorted.size()));
    if (rank < 1.0) rank = 1.0;
    double n = static_cast<double>(sorted.size());
    if (rank > n) rank = n;
    return sorted[static_cast<size_t>(rank) - 1];
}

// 中位数（标准定义）：n 为奇数取中间元素；n 为偶数取中间两元素的平均。
// 旧实现直接用 sorted[n/2]，偶数样本时偏向上半侧。
static double median(const std::vector<double>& sorted) {
    if (sorted.empty()) return 0.0;
    size_t n = sorted.size();
    if (n % 2 == 1) return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

// ── 主函数 ─────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "用法: " << argv[0] << " <host> <port> <path> [count] [msg_size] [rounds] [warmup]\n"
                  << "示例: " << argv[0] << " localhost 9443 /echo 1000 256 3\n"
                  << "默认: count=1000 (样本量太小分位数无区分度), msg_size=256, rounds=1, warmup=count/10\n";
        return 1;
    }

    std::string host = argv[1];
    int port = std::stoi(argv[2]);
    std::string path = argv[3];
    int count = (argc > 4) ? std::stoi(argv[4]) : 1000;
    int msgSize = (argc > 5) ? std::stoi(argv[5]) : 256;
    int rounds = (argc > 6) ? std::stoi(argv[6]) : 1;
    // 预热条数：默认 count/10（至少 50），只用于让 TLS 记录层/内核缓冲进入稳态
    int warmup = (argc > 7) ? std::stoi(argv[7]) : std::max(50, count / 10);
    if (count <= 0) { std::cerr << "count 必须 > 0\n"; return 1; }
    if (warmup < 0) warmup = 0;

    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║  WSS 基准客户端                        ║\n";
    std::cout << "╚══════════════════════════════════════╝\n";
    std::cout << "目标: wss://" << host << ":" << port << path << "\n";
    std::cout << "消息数: " << count << "  大小: " << msgSize << "B  轮次: " << rounds
              << "  预热: " << warmup << " 条/轮 (不计入统计)\n\n";

    // 生成测试负载
    std::string payload(msgSize, 'x');

    std::vector<double> allLatencies;

    for (int round = 0; round < rounds; ++round) {
        if (rounds > 1) std::cout << "--- 第 " << (round+1) << "/" << rounds << " 轮 ---\n";

        // 1. TCP 连接
        int fd = tcpConnect(host, port);
        if (fd < 0) { std::cerr << "TCP连接失败\n"; return 1; }

        // 2. TLS 握手
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        // 明确关闭证书校验（L10）：这是**基准测试客户端**，不是生产客户端范例。
        // 生产客户端必须调用 SSL_CTX_set_verify(..., SSL_VERIFY_PEER, ...) 并加载 CA。
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host.c_str());

        if (SSL_connect(ssl) != 1) {
            std::cerr << "TLS 握手失败\n";
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return 1;
        }

        // 3. WebSocket 升级
        std::string upgradeReq =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        if (!writeAll(ssl, upgradeReq.data(), upgradeReq.size())) {
            std::cerr << "发送升级请求失败\n";
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return 1;
        }

        // 读取 101 响应
        char respBuf[8192];
        int nr = SSL_read(ssl, respBuf, sizeof(respBuf) - 1);
        if (nr <= 0) {
            std::cerr << "读取升级响应失败\n";
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return 1;
        }
        respBuf[nr] = '\0';
        std::string response(respBuf, nr);
        if (response.find("101") == std::string::npos) {
            std::cerr << "升级失败: " << response.substr(0, 100) << "\n";
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return 1;
        }

        // 4. 创建解析器（客户端模式：不要求 mask）
        http::wss::WebSocketStreamParser parser(false);
        parser.setOpenMode();

        // 5. 单条消息往返：发送 → 等 echo → 返回 RTT(ms)；出错返回 -1
        auto exchange = [&](int seq) -> double {
            auto t1 = std::chrono::steady_clock::now();

            // 发送
            std::string msg = "bench-" + std::to_string(seq) + "-" + payload;
            if (msg.size() > static_cast<size_t>(msgSize))
                msg.resize(msgSize);
            auto frame = http::wss::WebSocketCodec::buildClientTextFrame(msg);
            if (!writeAll(ssl, frame.data(), frame.size())) {
                std::cerr << "发送消息 " << seq << " 失败\n";
                return -1.0;
            }

            // 接收（循环读取直到收到完整帧）
            while (true) {
                char buf[65536];
                int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) { std::cerr << "读取响应 " << seq << " 失败\n"; return -1.0; }

                std::vector<http::wss::WsFrame> frames;
                std::string acceptResp;
                parser.feed(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                           &acceptResp, &frames);

                for (const auto& f : frames) {
                    if (f.opcode == 0x1 || f.opcode == 0x2) {  // text or binary
                        auto t2 = std::chrono::steady_clock::now();
                        return std::chrono::duration<double, std::milli>(t2 - t1).count();
                    } else if (f.opcode == 0x9) {  // ping → pong
                        auto pong = http::wss::WebSocketCodec::buildClientFrame(0xA, f.payload);
                        writeAll(ssl, pong.data(), pong.size());
                    }
                }
            }
        };

        // 6. 预热（不计入统计）：首几条消息受 TLS 记录层/内核缓冲影响，偏差大
        int warmFail = 0;
        for (int i = 0; i < warmup; ++i) {
            if (exchange(-1 - i) < 0.0) { ++warmFail; break; }
        }
        if (warmup > 0) {
            std::cout << "  预热: " << (warmup - warmFail) << "/" << warmup
                      << " 条完成 (不计入统计)\n";
        }

        // 7. 正式测量 N 条消息 RTT
        std::vector<double> latencies;
        auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < count; ++i) {
            double ms = exchange(i);
            if (ms < 0.0) break;
            latencies.push_back(ms);
        }

        auto tEnd = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(tEnd - t0).count();

        // 8. 关闭连接
        auto closeFrame = http::wss::WebSocketCodec::buildClientFrame(0x8, {});
        writeAll(ssl, closeFrame.data(), closeFrame.size());
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);

        // 统计
        std::sort(latencies.begin(), latencies.end());
        allLatencies.insert(allLatencies.end(), latencies.begin(), latencies.end());

        std::cout << "  完成: " << latencies.size() << "/" << count << " 消息\n";
        std::cout << "  耗时: " << elapsed << "s  |  ";
        if (elapsed > 0) std::cout << "吞吐: " << static_cast<int>(latencies.size() / elapsed) << " msg/s\n";
        if (!latencies.empty()) {
            double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            std::cout << "  延迟: avg=" << avg
                      << "ms  p50=" << median(latencies)
                      << "ms  p90=" << percentile(latencies, 0.90)
                      << "ms  p99=" << percentile(latencies, 0.99)
                      << "ms  max=" << latencies.back() << "ms\n";
        }
    }

    // ── 汇总 ──
    if (!allLatencies.empty()) {
        std::sort(allLatencies.begin(), allLatencies.end());
        double avg = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0) / allLatencies.size();
        std::cout << "\n╔══════════════════════════════════════╗\n";
        std::cout << "║  最终结果                              ║\n";
        std::cout << "╚══════════════════════════════════════╝\n";
        std::cout << "消息大小: " << msgSize << " B\n";
        std::cout << "总消息数: " << allLatencies.size() << "\n";
        std::cout << "延迟 avg: " << avg << " ms\n";
        std::cout << "延迟 p50: " << median(allLatencies) << " ms  (中位数, 标准定义)\n";
        std::cout << "延迟 p90: " << percentile(allLatencies, 0.90) << " ms\n";
        std::cout << "延迟 p99: " << percentile(allLatencies, 0.99) << " ms\n";
        std::cout << "延迟 max: " << allLatencies.back() << " ms\n";
        std::cout << "备注: 分位口径 nearest-rank idx=ceil(q*n)-1, 样本 n=" << allLatencies.size()
                  << ", 预热不计入\n";
    }

    return 0;
}
