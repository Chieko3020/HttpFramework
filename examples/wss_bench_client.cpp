// wss_bench_client.cpp — WSS 性能基准客户端
// 用法: wss_bench_client <host> <port> <path> <count> <msg_size>
// 示例: wss_bench_client localhost 9443 /echo 1000 256
//
// 单 TLS 连接、多消息发送，精确测量 WebSocket 吞吐量和延迟

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

// ── 主函数 ─────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "用法: " << argv[0] << " <host> <port> <path> <count> [msg_size] [rounds]\n"
                  << "示例: " << argv[0] << " localhost 9443 /echo 1000 256 3\n";
        return 1;
    }

    std::string host = argv[1];
    int port = std::stoi(argv[2]);
    std::string path = argv[3];
    int count = std::stoi(argv[4]);
    int msgSize = (argc > 5) ? std::stoi(argv[5]) : 256;
    int rounds = (argc > 6) ? std::stoi(argv[6]) : 1;

    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║  WSS 基准客户端                        ║\n";
    std::cout << "╚══════════════════════════════════════╝\n";
    std::cout << "目标: wss://" << host << ":" << port << path << "\n";
    std::cout << "消息数: " << count << "  大小: " << msgSize << "B  轮次: " << rounds << "\n\n";

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

        // 5. 发送 N 条消息并测量 RTT
        std::vector<double> latencies;
        auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < count; ++i) {
            auto t1 = std::chrono::steady_clock::now();

            // 发送
            std::string msg = "bench-" + std::to_string(i) + "-" + payload;
            if (msg.size() > static_cast<size_t>(msgSize))
                msg.resize(msgSize);
            auto frame = http::wss::WebSocketCodec::buildClientTextFrame(msg);
            if (!writeAll(ssl, frame.data(), frame.size())) {
                std::cerr << "发送消息 " << i << " 失败\n";
                break;
            }

            // 接收（循环读取直到收到完整帧）
            bool gotResponse = false;
            while (!gotResponse) {
                char buf[65536];
                int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) { std::cerr << "读取响应 " << i << " 失败\n"; break; }

                std::vector<http::wss::WsFrame> frames;
                std::string acceptResp;
                parser.feed(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                           &acceptResp, &frames);

                for (const auto& f : frames) {
                    if (f.opcode == 0x1 || f.opcode == 0x2) {  // text or binary
                        auto t2 = std::chrono::steady_clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        latencies.push_back(ms);
                        gotResponse = true;
                    } else if (f.opcode == 0x9) {  // ping → pong
                        auto pong = http::wss::WebSocketCodec::buildClientFrame(0xA, f.payload);
                        writeAll(ssl, pong.data(), pong.size());
                    }
                }
            }
        }

        auto tEnd = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(tEnd - t0).count();

        // 6. 关闭连接
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
            std::cout << "  延迟: avg=" << avg << "ms  p50=" << latencies[latencies.size()/2]
                      << "ms  p99=" << latencies[static_cast<size_t>(latencies.size()*0.99)] << "ms\n";
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
        std::cout << "延迟 p50: " << allLatencies[allLatencies.size()/2] << " ms\n";
        std::cout << "延迟 p99: " << allLatencies[static_cast<size_t>(allLatencies.size()*0.99)] << " ms\n";
    }

    return 0;
}
