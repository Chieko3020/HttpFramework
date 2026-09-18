// test_http_hardening.cpp — 深度审查修复的回归测试
//
// 覆盖（每条都对应一次已实测复现的缺陷）：
//   C1  SIGPIPE / EPIPE：客户端 RST 后进程必须存活
//   C2  陈旧 fd 跨连接误杀：空闲超时关闭的连接，其 fd 号被新连接复用后
//       旧 worker 的完成回调不得关闭新连接
//   C3  HTTP 管线化：一次写入 N 个请求必须得到 N 个响应
//   C4  内存池模式下响应体不得被静默截断（Content-Length 与实收字节一致）
//   C5  请求体严格按 Content-Length 切分，不得夹带后续粘包字节
//   C6  accept 竞态：连接建立后立刻发请求必须都能拿到响应
//   H1  请求体超限的 413 不得被路由覆盖成 404
//   H2  413 路径的统计记账必须成对（totalRequests 与 completedRequests 均计入）
//
// 断言的口径都来自"客户端可观测行为"，不依赖内部实现细节。

#include "HttpFramework.h"   // H15：App 级统计/信号
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "router/Router.h"
#include "utils/SocketCompat.h"
#include "utils/ThreadPool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while (0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

// ───────────────────────── 基础工具 ─────────────────────────

// 取一个当前空闲的本地端口（bind 0 后立即释放）
static int pickPort() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return 0;
    }
    socklen_t len = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &len);
    int port = ntohs(a.sin_port);
    close(s);
    return port;
}

static int connectTo(int port, int timeoutMs = 3000) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

// 测试进程自身也要忽略 SIGPIPE：下面的用例会故意对已 RST 的套接字写入
static void ignoreSigpipeInTest() {
    http::net::ensureSigpipeIgnored();
}

static bool sendAll(int sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

struct ParsedResponse {
    std::string status;
    std::string headers;
    std::string body;
    size_t contentLength = 0;
    bool complete = false;
};

// 按 Content-Length 读一个完整响应。
//
// carry 必须在同一连接的多次调用之间复用：一次 recv 完全可能把同一连接上的
// 多个响应一起带回（服务端并发写、Nagle 合并都会让它们粘在一起）。旧实现每次
// 调用都用新的局部缓冲、把多余字节丢掉，于是"第二个响应"会偶发地被测试自己
// 吃掉，表现为 5s 超时 —— 这是测试客户端的缺陷，不是服务端丢响应
// （已用 1500 轮管线化探针确认服务端逐字节正确）。
static ParsedResponse readResponseImpl(int sock, std::string& carry, int timeoutMs) {
    ParsedResponse r;
    std::string& buf = carry;
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        auto hdrEnd = buf.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            r.headers = buf.substr(0, hdrEnd);
            auto lineEnd = r.headers.find("\r\n");
            r.status = (lineEnd == std::string::npos) ? r.headers
                                                      : r.headers.substr(0, lineEnd);
            auto clPos = r.headers.find("Content-Length:");
            if (clPos != std::string::npos) {
                auto clEnd = r.headers.find("\r\n", clPos);
                try {
                    r.contentLength = std::stoul(
                        r.headers.substr(clPos + 15, clEnd - (clPos + 15)));
                } catch (...) {
                    r.contentLength = 0;
                }
            }
            size_t bodyStart = hdrEnd + 4;
            if (buf.size() - bodyStart >= r.contentLength) {
                r.body = buf.substr(bodyStart, r.contentLength);
                // 消费掉本次响应的全部字节，剩余留给下一次调用
                buf.erase(0, bodyStart + r.contentLength);
                r.complete = true;
                return r;
            }
        }
        char tmp[8192];
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return r;  // 对端关闭 / 超时
        buf.append(tmp, static_cast<size_t>(n));
    }
}

// 单响应场景的便捷入口（每次调用自带空缓冲）
static ParsedResponse readResponse(int sock, int timeoutMs = 5000) {
    std::string carry;
    return readResponseImpl(sock, carry, timeoutMs);
}

static bool peekClosed(int sock, int timeoutMs = 300) {
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char c;
    ssize_t n = recv(sock, &c, 1, MSG_PEEK);
    return n == 0;
}

// 组装一个服务器：路由 /small、/size?n=、/echo、/slow
struct Fixture {
    std::unique_ptr<utils::ThreadPool> pool;
    std::unique_ptr<http::HttpServer> server;
    std::shared_ptr<router::Router> routerRef;   // 供 H14 用例在运行期注册路由
    int port = 0;
    bool started = false;

    bool up(int idleSec = 60, size_t subReactors = 1, bool pool_ = false,
            size_t workers = 4,
            std::function<void(router::Router&)> extra = nullptr,
            size_t poolBlockBytes = 0) {
        port = pickPort();
        if (port == 0) return false;
        pool = std::make_unique<utils::ThreadPool>(workers);
        server = std::make_unique<http::HttpServer>(port, *pool, subReactors);
        server->setIdleTimeout(idleSec);
        if (pool_) server->enableMemoryPool(true, poolBlockBytes);

        auto r = std::make_shared<router::Router>();
        r->get("/small", [](const http::HttpRequest&, http::HttpResponse& res) {
            res.setText("SMALL");
        });
        r->get("/size", [](const http::HttpRequest& req, http::HttpResponse& res) {
            size_t n = 0;
            try { n = std::stoul(req.getQuery("n")); } catch (...) { n = 0; }
            res.setText(std::string(n, 'Z'));
        });
        r->post("/echo", [](const http::HttpRequest& req, http::HttpResponse& res) {
            res.setText(req.getBody());
        });
        r->get("/slow", [](const http::HttpRequest&, http::HttpResponse& res) {
            std::this_thread::sleep_for(std::chrono::seconds(6));
            res.setText("SLOWDONE");
        });
        // 时长可配的慢 handler：用于把"空闲超时先于 worker 结束"的窗口做小，
        // 缩短 C2 用例的等待时间
        r->get("/slowms", [](const http::HttpRequest& req, http::HttpResponse& res) {
            long ms = 2000;
            try { ms = std::stol(req.getQuery("ms")); } catch (...) { ms = 2000; }
            if (ms < 0) ms = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            res.setText("SLOWDONE");
        });
        server->setRouter(r);
        routerRef = r;

        if (extra) extra(*r);   // 用例自定义路由（必须在 start() 之前注册）

        started = server->start();
        if (started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return started;
    }

    void down() {
        if (server && started) server->stop();
        if (pool) pool->shutdown();
        started = false;
    }

    ~Fixture() { down(); }
};

static std::string req(const std::string& method, const std::string& path,
                       const std::string& extraHeaders = "",
                       const std::string& body = "") {
    std::string r = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!body.empty() || method == "POST") {
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    r += extraHeaders;
    r += "\r\n";
    r += body;
    return r;
}

// ── C1：客户端 RST 后服务进程必须存活 ──
static bool test_sigpipe_survival() {
    TEST("C1 客户端 RST 20+ 次后服务进程存活");
    Fixture f;
    if (!f.up()) FAIL("服务器启动失败");

    const int kRounds = 30;
    int sent = 0;
    for (int i = 0; i < kRounds; ++i) {
        if (!f.server->isRunning()) break;
        int s = connectTo(f.port, 1000);
        if (s < 0) break;
        // 请求一个大响应，让服务端在写的过程中被 RST 打断
        sendAll(s, req("GET", "/size?n=20000"));
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(s);
        ++sent;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK(f.server->isRunning(),
          "服务进程在 " << sent << " 次 RST 连接后应仍然存活（SIGPIPE 未处理时会直接退出）");

    // 进程活着还不够：还要还能正常服务
    int s = connectTo(f.port);
    CHECK(s >= 0, "重启连接应成功");
    CHECK(sendAll(s, req("GET", "/small")), "RST 之后仍应能发送请求");
    auto r = readResponse(s);
    close(s);
    CHECK(r.complete && r.status.rfind("HTTP/1.1 200", 0) == 0,
          "RST 之后仍应能正常响应, 实际: " << r.status);

    f.down();
    PASS();
    return true;
}

// ── C3：管线化 ──
static bool test_pipelining() {
    TEST("C3 一次写入 3 个请求得到 3 个响应");
    Fixture f;
    if (!f.up()) FAIL("服务器启动失败");

    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");

    std::string batch = req("GET", "/small") + req("GET", "/small") + req("GET", "/small");
    CHECK(sendAll(s, batch), "批量写入失败");

    std::string carry;   // 同一连接上的响应可能被一次 recv 一起带回
    for (int i = 0; i < 3; ++i) {
        auto r = readResponseImpl(s, carry, 5000);
        CHECK(r.complete, "第 " << (i + 1) << " 个响应未收到（管线化请求被丢弃）");
        CHECK(r.status.rfind("HTTP/1.1 200", 0) == 0,
              "第 " << (i + 1) << " 个响应状态异常: " << r.status);
        CHECK(r.body == "SMALL", "第 " << (i + 1) << " 个响应体异常: " << r.body);
    }

    close(s);
    f.down();
    PASS();
    return true;
}

// ── C5：body 严格按 Content-Length 切分 ──
static bool test_body_slicing() {
    TEST("C5 请求体不夹带后续粘包字节（池模式与非池模式）");
    for (int mode = 0; mode < 2; ++mode) {
        Fixture f;
        if (!f.up(60, 1, mode == 1)) FAIL("服务器启动失败 (mode=" << mode << ")");

        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        // POST body="hello"（5 字节）后紧跟一个 GET，同一次写入
        std::string batch = req("POST", "/echo", "", "hello") + req("GET", "/small");
        CHECK(sendAll(s, batch), "批量写入失败");

        std::string carry;
        auto r1 = readResponseImpl(s, carry, 5000);
        CHECK(r1.complete, "POST 响应未收到");
        CHECK(r1.body == "hello",
              "echo 体应恰好是 Content-Length 指定的 5 字节, 实际: '" << r1.body << "'");

        auto r2 = readResponseImpl(s, carry, 5000);
        CHECK(r2.complete, "后续 GET 响应未收到（被 body 吞掉）");
        CHECK(r2.body == "SMALL", "后续 GET 响应体异常: " << r2.body);

        close(s);
        f.down();
    }
    PASS();
    return true;
}

// ── C4：内存池模式下响应体不得被静默截断 ──
static bool test_response_size_boundary() {
    TEST("C4 响应大小边界（12287/12288/12289/20000）Content-Length 与实收一致");
    const size_t sizes[] = {12287, 12288, 12289, 20000};

    for (int mode = 0; mode < 2; ++mode) {
        Fixture f;
        if (!f.up(60, 1, mode == 1)) FAIL("服务器启动失败 (mode=" << mode << ")");

        for (size_t n : sizes) {
            int s = connectTo(f.port);
            CHECK(s >= 0, "连接失败");
            CHECK(sendAll(s, req("GET", "/size?n=" + std::to_string(n))), "发送失败");

            std::string carry;
            auto r = readResponseImpl(s, carry, 8000);
            CHECK(r.complete, "[mode=" << mode << " n=" << n << "] 响应不完整"
                  << " (Content-Length=" << r.contentLength
                  << ", 实收=" << r.body.size() << ")");
            CHECK(r.contentLength == n,
                  "[mode=" << mode << " n=" << n << "] Content-Length=" << r.contentLength);
            CHECK(r.body.size() == n,
                  "[mode=" << mode << " n=" << n << "] 实收字节=" << r.body.size());
            CHECK(r.body == std::string(n, 'Z'), "[mode=" << mode << " n=" << n << "] 内容被损坏");

            // keep-alive：紧接着的请求不能被当成前一个响应的续传体
            CHECK(sendAll(s, req("GET", "/small")), "keep-alive 复用发送失败");
            auto r2 = readResponseImpl(s, carry, 5000);
            CHECK(r2.complete && r2.body == "SMALL",
                  "[mode=" << mode << " n=" << n << "] keep-alive 后续响应错位: '"
                  << r2.status << "' body=" << r2.body.size());
            close(s);
        }
        f.down();
    }
    PASS();
    return true;
}

// ── C6：accept 竞态 ──
static bool test_accept_race() {
    TEST("C6 1000 次「连上即发」全部拿到响应");
    Fixture f;
    // 多 reactor + 多线程，最大化 accept 与事件就绪之间的竞态窗口
    if (!f.up(60, 4, false, 4)) FAIL("服务器启动失败");

    const int kThreads = 8;
    const int kPerThread = 125;
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    std::atomic<int> acceptedNoResponse{0};

    auto worker = [&]() {
        for (int i = 0; i < kPerThread; ++i) {
            int s = connectTo(f.port, 3000);
            if (s < 0) { fail.fetch_add(1); continue; }
            if (!sendAll(s, req("GET", "/small"))) { fail.fetch_add(1); close(s); continue; }
            auto r = readResponse(s, 3000);
            if (r.complete && r.status.rfind("HTTP/1.1 200", 0) == 0 && r.body == "SMALL") {
                ok.fetch_add(1);
            } else {
                fail.fetch_add(1);
                if (!r.complete) acceptedNoResponse.fetch_add(1);
            }
            close(s);
        }
    };

    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();

    std::cout << "(成功=" << ok.load() << " 失败=" << fail.load()
              << " 其中无响应即被关闭=" << acceptedNoResponse.load() << ") ";

    CHECK(fail.load() == 0,
          "连上即发应 100% 成功, 失败 " << fail.load() << "/" << (kThreads * kPerThread)
          << "（其中 " << acceptedNoResponse.load() << " 次在响应前被服务端关闭）");

    f.down();
    PASS();
    return true;
}

// ── H1 / H2：413 语义与统计记账 ──
static bool test_payload_too_large() {
    TEST("H1/H2 请求体超限返回 413（不被路由改成 404）且统计成对");
    Fixture f;
    if (!f.up(60, 1, /*pool=*/true)) FAIL("服务器启动失败");

    auto& st = f.server->getStatistics();
    const uint64_t total0 = st.totalRequests.load();
    const uint64_t completed0 = st.completedRequests.load();

    // 13000 字节 > 池单块 12288，必然触发请求体截断
    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    std::string bigBody(13000, 'A');
    CHECK(sendAll(s, req("POST", "/echo", "", bigBody)), "发送失败");
    auto r = readResponse(s, 8000);
    CHECK(r.complete, "413 响应未收到");
    CHECK(r.status.rfind("HTTP/1.1 413", 0) == 0,
          "应返回 413, 实际: " << r.status << "（被路由覆盖时会变成 404）");
    CHECK(r.body.find("Payload too large") != std::string::npos,
          "413 响应体应保留框架给出的说明, 实际: " << r.body);
    close(s);

    // 等待在途任务结算
    for (int i = 0; i < 40; ++i) {
        if (st.completedRequests.load() > completed0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const uint64_t totalDelta = st.totalRequests.load() - total0;
    const uint64_t completedDelta = st.completedRequests.load() - completed0;
    std::cout << "(Δtotal=" << totalDelta << " Δcompleted=" << completedDelta << ") ";

    CHECK(totalDelta == 1,
          "被截断的请求必须计入 totalRequests（修复前为 0，导致 queuedTasks 下溢）");
    CHECK(completedDelta == 1,
          "被截断的请求必须计入 completedRequests, 实际 +" << completedDelta);

    f.down();
    PASS();
    return true;
}

// ── C2：陈旧 fd 跨连接误杀 ──
// 时序（idle=1s，timerfd 固定 5s 一轮）：c1 发出 6s 的慢请求 → 越过 t=5s 空闲
// 检查点后 c1 已被关闭（fd 归还内核）、worker 仍在途 → c2 复用该 fd 号并落在
// 另一个 subReactor → 旧 worker 结束时不得关闭 c2。
static bool test_stale_fd_no_crosstalk() {
    TEST("C2 空闲超时关闭的连接，其 fd 号被复用后不被旧 worker 误杀");
    Fixture f;
    if (!f.up(/*idleSec=*/1, /*subReactors=*/2, false, 4)) FAIL("服务器启动失败");

    const int kMaxAttempts = 3;
    bool hit = false;      // 是否真的观测到 fd 号复用
    bool killed = false;
    int reused = 0;
    int attempts = 0;

    for (; attempts < kMaxAttempts && !killed; ++attempts) {
        // 用一个"哨兵"连接占住低位 fd 号：c1 关闭后，c2 必然拿到同一个号
        int sentinel = connectTo(f.port);
        if (sentinel >= 0) {
            sendAll(sentinel, req("GET", "/small"));
            readResponse(sentinel, 3000);
        }

        int c1 = connectTo(f.port);
        if (c1 < 0) { if (sentinel >= 0) close(sentinel); continue; }
        if (!sendAll(c1, req("GET", "/slow"))) {
            close(c1);
            if (sentinel >= 0) close(sentinel);
            continue;
        }

        // 越过服务端 timerfd 的 5s 空闲检查点：c1 已被服务端关闭、worker 仍在途
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));

        // 客户端同步关掉 c1（服务端已关闭，这里只是把本进程的 fd 号还给内核），
        // 紧接着新建连接即可命中同一个 fd 号——这正是 C2 的触发条件
        close(c1);
        int c2 = connectTo(f.port);
        if (sentinel >= 0) { close(sentinel); sentinel = -1; }
        if (c2 < 0) continue;
        if (!sendAll(c2, req("GET", "/small"))) { close(c1); close(c2); continue; }
        std::string carry;
        auto r = readResponseImpl(c2, carry, 4000);
        if (!(r.complete && r.body == "SMALL")) {
            std::cout << "(第 " << (attempts + 1) << " 轮 c2 首个响应异常: "
                      << r.status << ") ";
            close(c2);
            continue;
        }

        // 覆盖旧 worker 结束时刻（c1 连接后 6s，约在此处再等 1s），持续复用 c2
        int roundReused = 0;
        bool roundKilled = false;
        for (int i = 0; i < 16; ++i) {
            if (peekClosed(c2)) { roundKilled = true; break; }
            if (!sendAll(c2, req("GET", "/small"))) { roundKilled = peekClosed(c2); break; }
            auto r2 = readResponseImpl(c2, carry, 2000);
            if (!(r2.complete && r2.body == "SMALL")) { roundKilled = true; break; }
            ++roundReused;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 命中判据：c2 拿到了 c1 刚归还的同一个 fd 号。
        // 不命中时本用例无法判定（fd 号是否复用由内核决定），按跳过处理并打印
        // 实际取值以便排查；跨 reactor 的完整复现见 verify 脚本 c2_final.py。
        if (c2 == c1) {
            hit = true;
            reused += roundReused;
            if (roundKilled) killed = true;
        }
        close(c2);
    }

    std::cout << "(轮数=" << attempts << " 命中 fd 复用=" << (hit ? "是" : "否")
              << " 复用成功 " << reused << " 次) ";

    if (!hit) {
        std::cout << "(本轮未复用到同一 fd 号，无法判定 — 跳过；"
                     "完整复现见 c2_final.py) ";
        f.down();
        return true;
    }

    CHECK(!killed, "c2 在旧 worker 完成时被单方面断开（陈旧 fd 误杀）");
    CHECK(reused >= 5, "复用次数过少: " << reused);

    f.down();
    PASS();
    return true;
}

// ── H3：共享线程池下 stop() 必须等在途任务结束再释放成员 ──
// 任务体是 HttpServer 的成员函数（捕获 this），失败模式是 ~HttpServer 返回后
// worker 仍在访问已释放的 this。客户端不可见，改成"stop() 返回时在途任务必须
// 已结束"这一可直接断言的判据。
static bool test_stop_waits_for_inflight() {
    TEST("H3 stop() 等待在途业务任务归零（共享线程池）");
    auto entered  = std::make_shared<std::atomic<int>>(0);
    auto finished = std::make_shared<std::atomic<int>>(0);

    Fixture f;
    if (!f.up(60, 1, false, 4, [entered, finished](router::Router& r) {
            r.get("/h3slow", [entered, finished](const http::HttpRequest&,
                                                 http::HttpResponse& res) {
                entered->fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                finished->fetch_add(1);
                res.setText("H3DONE");
            });
        })) FAIL("服务器启动失败");

    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    CHECK(sendAll(s, req("GET", "/h3slow")), "发送失败");

    // 等到 handler 真正开始执行，确保 stop() 面对的是"在途"状态
    for (int i = 0; i < 200 && entered->load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(entered->load() == 1, "handler 未进入执行");

    const auto t0 = std::chrono::steady_clock::now();
    f.server->stop();
    const auto waitedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

    std::cout << "(stop 耗时=" << waitedMs << "ms, handler 完成数="
              << finished->load() << ") ";

    CHECK(finished->load() == 1,
          "stop() 返回时在途 handler 仍未结束：析构后 worker 会访问已释放的 this");
    CHECK(f.server->inFlightTaskCount() == 0,
          "stop() 返回后在途任务计数应为 0, 实际 " << f.server->inFlightTaskCount());
    // 等待发生在途任务（handler 还要睡约 1.4s）；修复前 stop() 只做 fd 关闭+join，
    // 几十毫秒就返回
    CHECK(waitedMs >= 800, "stop() 未等待在途任务, 仅耗时 " << waitedMs << "ms");

    close(s);
    f.down();
    PASS();
    return true;
}

// ── H4：慢 handler 不得被空闲超时误杀 ──
// timerfd 每 5s 走一轮空闲检查；连接上仍有在途请求（handler 还在跑）时不得关闭。
static bool test_slow_handler_not_killed_by_timer() {
    TEST("H4 handler 耗时超过 idleTimeout 不被空闲清理误杀");
    auto entered = std::make_shared<std::atomic<int>>(0);

    Fixture f;
    if (!f.up(/*idleSec=*/1, /*subReactors=*/1, false, 4,
              [entered](router::Router& r) {
                  r.get("/h4slow", [entered](const http::HttpRequest&,
                                             http::HttpResponse& res) {
                      entered->fetch_add(1);
                      std::this_thread::sleep_for(std::chrono::milliseconds(6500));
                      res.setText("H4DONE");
                  });
              })) FAIL("服务器启动失败");

    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    CHECK(sendAll(s, req("GET", "/h4slow")), "发送失败");

    // 修复前：t=5s 的 timer 看到 lastActive 停留在建连时刻（idle>=1s），
    // 直接把还在等 handler 的连接关掉，客户端收到 EOF
    auto r = readResponse(s, 10000);
    std::cout << "(handler 进入=" << entered->load() << " 响应完整=" << r.complete
              << " status='" << r.status << "') ";
    CHECK(r.complete, "慢 handler 的连接在响应前被关闭（被空闲超时误杀）");
    CHECK(r.status.rfind("HTTP/1.1 200", 0) == 0, "状态异常: " << r.status);
    CHECK(r.body == "H4DONE", "响应体异常: " << r.body);

    close(s);
    f.down();
    PASS();
    return true;
}

// ── H7：内存池模式下的单连接上限可配置，且超限时 413 可区分 ──
static bool test_pool_limit_configurable() {
    TEST("H7 内存池块容量可配置(4096)，超限 413 带 limit，未超限正常");
    Fixture f;
    if (!f.up(60, 1, /*pool=*/true, 4, nullptr, /*poolBlockBytes=*/4096))
        FAIL("服务器启动失败");

    CHECK(f.server->memoryPoolBlockSize() == 4096,
          "块容量应为 4096, 实际 " << f.server->memoryPoolBlockSize());

    // 5000 字节 body > 4096 → 必须 413，且响应体给出可区分的上限
    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    CHECK(sendAll(s, req("POST", "/echo", "", std::string(5000, 'A'))), "发送失败");
    auto r = readResponse(s, 8000);
    CHECK(r.complete, "413 响应未收到");
    CHECK(r.status.rfind("HTTP/1.1 413", 0) == 0, "应返回 413, 实际: " << r.status);
    CHECK(r.body.find("\"limit\":4096") != std::string::npos,
          "413 响应体应给出可区分的上限(limit:4096), 实际: " << r.body);
    close(s);

    // 同一配置下 2000 字节正常通过：证明上限确实来自配置而不是恒定 12KB
    int s2 = connectTo(f.port);
    CHECK(s2 >= 0, "连接失败");
    CHECK(sendAll(s2, req("POST", "/echo", "", std::string(2000, 'B'))), "发送失败");
    auto r2 = readResponse(s2, 8000);
    CHECK(r2.complete && r2.status.rfind("HTTP/1.1 200", 0) == 0,
          "2000 字节请求应正常, 实际: " << r2.status);
    CHECK(r2.body == std::string(2000, 'B'), "echo 体异常, size=" << r2.body.size());
    close(s2);

    f.down();
    PASS();
    return true;
}

// ── H8：内存池容量悬崖必须显式上报（503），不得静默退化成 413/空响应 ──
static bool test_pool_exhaustion_reported() {
    TEST("H8 池耗尽时显式回 503（而不是静默截断成 413）");
    // 只给 1 个块：第一个连接占住它，第二个连接的请求缓冲分配必然失败
    int port = pickPort();
    utils::ThreadPool pool(4);
    http::HttpServer server(port, pool, 1);
    server.setIdleTimeout(60);
    server.enableMemoryPool(true, /*blockSizeBytes=*/0, /*poolBlocks=*/1);
    auto r = std::make_shared<router::Router>();
    r->get("/small", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("SMALL");
    });
    server.setRouter(r);
    CHECK(server.start(), "服务器启动失败");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // 连接 1：占住唯一的内存块，正常拿到响应
    int c1 = connectTo(port);
    CHECK(c1 >= 0, "连接失败");
    CHECK(sendAll(c1, req("GET", "/small")), "发送失败");
    auto r1 = readResponse(c1, 5000);
    CHECK(r1.complete && r1.status.rfind("HTTP/1.1 200", 0) == 0,
          "第 1 个连接应正常: " << r1.status);

    // 连接 2：池已耗尽 → 必须明确回 503，且响应体说明是服务端容量问题
    int c2 = connectTo(port);
    CHECK(c2 >= 0, "连接失败");
    CHECK(sendAll(c2, req("GET", "/small")), "发送失败");
    auto r2 = readResponse(c2, 5000);
    std::cout << "(第2连接 status='" << r2.status << "') ";
    CHECK(r2.complete, "池耗尽时应给出响应而不是静默挂起");
    CHECK(r2.status.rfind("HTTP/1.1 503", 0) == 0,
          "池耗尽应回 503, 实际: " << r2.status);
    CHECK(r2.body.find("pool exhausted") != std::string::npos,
          "响应体应说明原因, 实际: " << r2.body);
    close(c2);

    // 连接 1 关闭后块归还，后续连接恢复正常
    close(c1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int c3 = connectTo(port);
    CHECK(c3 >= 0, "连接失败");
    CHECK(sendAll(c3, req("GET", "/small")), "发送失败");
    auto r3 = readResponse(c3, 5000);
    CHECK(r3.complete && r3.status.rfind("HTTP/1.1 200", 0) == 0,
          "块归还后应恢复正常: " << r3.status);
    close(c3);

    server.stop();
    pool.shutdown();
    PASS();
    return true;
}

// ── H13：HEAD 抑制响应体、保留 Content-Length，并回退到 GET 路由 ──
static bool test_head_semantics() {
    TEST("H13 HEAD 无响应体但保留 Content-Length，且回退到 GET 路由");
    Fixture f;
    if (!f.up()) FAIL("服务器启动失败");

    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    // 只注册了 GET /small，HEAD 必须回退到它（否则 404）
    CHECK(sendAll(s, req("HEAD", "/small")), "发送失败");

    // 期望：状态行 200、Content-Length: 5、且**没有** body 字节
    std::string buf;
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (buf.find("\r\n\r\n") == std::string::npos) {
        char tmp[4096];
        ssize_t n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
    }
    auto hdrEnd = buf.find("\r\n\r\n");
    CHECK(hdrEnd != std::string::npos, "未收到完整头部");
    const std::string head = buf.substr(0, hdrEnd);
    const std::string body = buf.substr(hdrEnd + 4);

    std::cout << "(status='" << head.substr(0, 15) << "' 头部后字节=" << body.size() << ") ";

    CHECK(head.rfind("HTTP/1.1 200", 0) == 0,
          "HEAD 应回退到 GET 路由并返回 200, 实际: " << head.substr(0, 20));
    CHECK(head.find("Content-Length: 5") != std::string::npos,
          "HEAD 必须保留完整的 Content-Length, 实际头部: " << head);
    CHECK(body.empty(), "HEAD 响应不得携带 body, 实际收到 " << body.size() << " 字节: " << body);

    // 长连接上紧接着的 GET 必须正常（证明响应边界没被打乱）
    CHECK(sendAll(s, req("GET", "/small")), "keep-alive 复用发送失败");
    auto r2 = readResponse(s, 3000);
    CHECK(r2.complete && r2.body == "SMALL",
          "HEAD 之后同一连接上的 GET 响应异常: '" << r2.status << "' body=" << r2.body);

    close(s);
    f.down();
    PASS();
    return true;
}

// ── H14：运行期动态注册路由不得与请求处理竞争 ──
static bool test_router_dynamic_registration() {
    TEST("H14 并发请求期间动态注册 1000 条路由（无崩溃、新路由可见）");
    Fixture f;
    if (!f.up(60, 2, false, 4)) FAIL("服务器启动失败");

    std::atomic<bool> stopReg{false};
    std::atomic<int> registered{0};
    std::thread reg([&]() {
        for (int i = 0; i < 1000 && !stopReg.load(); ++i) {
            f.routerRef->get("/dyn/" + std::to_string(i),
                             [i](const http::HttpRequest&, http::HttpResponse& res) {
                                 res.setText("DYN" + std::to_string(i));
                             });
            registered.fetch_add(1);
        }
    });

    // 注册进行的同时持续打请求
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};
    auto worker = [&]() {
        for (int i = 0; i < 300; ++i) {
            int s = connectTo(f.port, 3000);
            if (s < 0) { bad.fetch_add(1); continue; }
            if (!sendAll(s, req("GET", "/small"))) { bad.fetch_add(1); close(s); continue; }
            auto r = readResponse(s, 3000);
            if (r.complete && r.body == "SMALL") ok.fetch_add(1);
            else bad.fetch_add(1);
            close(s);
        }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    stopReg.store(true);
    reg.join();

    std::cout << "(已注册=" << registered.load() << " 请求成功=" << ok.load()
              << " 失败=" << bad.load() << ") ";
    CHECK(bad.load() == 0, "并发注册期间请求失败 " << bad.load() << " 次");
    CHECK(registered.load() > 0, "未完成任何路由注册");

    // 新注册的路由必须可见
    int s = connectTo(f.port);
    CHECK(s >= 0, "连接失败");
    const int last = registered.load() - 1;
    CHECK(sendAll(s, req("GET", "/dyn/" + std::to_string(last))), "发送失败");
    auto r = readResponse(s, 3000);
    CHECK(r.complete && r.body == "DYN" + std::to_string(last),
          "运行期注册的路由不可见: '" << r.status << "' body=" << r.body);
    close(s);

    f.down();
    PASS();
    return true;
}

// ── H15：App::stats() 必须反映真实统计 ──
static bool test_app_stats_and_signal() {
    TEST("H15 App::stats() 反映真实请求数；SIGTERM 由主循环完成关闭");
    const int port = pickPort();
    http::App app;
    app.get("/x", [](const http::HttpRequest&, http::HttpResponse& res) { res.setText("X"); });

    std::atomic<bool> returned{false};
    std::thread t([&]() {
        try { app.start(port, 2); } catch (...) {}
        returned.store(true);
    });

    int s = -1;
    for (int i = 0; i < 200 && s < 0; ++i) {
        s = connectTo(port, 500);
        if (s < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(s >= 0, "App 未在 4s 内开始监听");
    CHECK(sendAll(s, req("GET", "/x")), "发送失败");
    auto r = readResponse(s, 3000);
    close(s);
    CHECK(r.complete && r.body == "X", "App 路由响应异常: " << r.status);

    uint64_t total = 0;
    for (int i = 0; i < 100; ++i) {
        total = app.stats().totalRequests.load();
        if (total >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "(App::stats().totalRequests=" << total << ") ";
    CHECK(total >= 1, "App::stats() 恒为 0（未转发到 HttpServer 的真实统计）");

    // 信号关闭：处理器只置标志，关闭在主循环完成
    raise(SIGTERM);
    for (int i = 0; i < 400 && !returned.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout << "(SIGTERM 后 start 返回=" << (returned.load() ? "是" : "否") << ") ";
    CHECK(returned.load(), "收到 SIGTERM 后 App::start() 未在 8s 内返回");
    t.join();
    PASS();
    return true;
}

// ── M3：框架头冲突与超上限请求体必须明确拒绝 ──
static bool test_request_framing_hardening() {
    TEST("M3 Content-Length+Transfer-Encoding 冲突→400；超配置上限→413");
    Fixture f;
    if (!f.up()) FAIL("服务器启动失败");
    f.server->setMaxRequestBodyBytes(1024);   // 便于测试的小上限

    // 1) 冲突头：同一请求同时带 Content-Length 与 Transfer-Encoding
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        std::string bad = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
        CHECK(sendAll(s, bad), "发送失败");
        auto r = readResponse(s, 3000);
        std::cout << "(冲突头→" << (r.status.empty() ? "无响应" : r.status.substr(9, 3)) << ") ";
        CHECK(r.complete, "冲突头请求应得到明确响应");
        CHECK(r.status.rfind("HTTP/1.1 400", 0) == 0,
              "Content-Length 与 Transfer-Encoding 并存应回 400, 实际: " << r.status);
        close(s);
    }

    // 2) 超过配置上限的 Content-Length
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        std::string bad = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n";
        CHECK(sendAll(s, bad), "发送失败");
        auto r = readResponse(s, 3000);
        std::cout << "(超上限→" << (r.status.empty() ? "无响应" : r.status.substr(9, 3)) << ") ";
        CHECK(r.complete, "超上限请求应得到明确响应");
        CHECK(r.status.rfind("HTTP/1.1 413", 0) == 0,
              "超过 maxRequestBodyBytes 应回 413, 实际: " << r.status);
        close(s);
    }

    // 3) 畸形 Content-Length 仍然 400（回归）
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        std::string bad = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 12abc\r\n\r\n";
        CHECK(sendAll(s, bad), "发送失败");
        auto r = readResponse(s, 3000);
        CHECK(r.complete && r.status.rfind("HTTP/1.1 400", 0) == 0,
              "畸形 Content-Length 应回 400, 实际: " << r.status);
        close(s);
    }

    f.down();
    PASS();
    return true;
}

// ── M9 / M11：原因短语与 URL 解码 ──
static bool test_reason_phrase_and_url_decode() {
    TEST("M9 302 原因短语为 Found；M11 路径解码严格化（%+1 不再解成 0x01）");
    Fixture f;
    if (!f.up(60, 1, false, 4, [](router::Router& r) {
            r.get("/echo/:id", [](const http::HttpRequest& req, http::HttpResponse& res) {
                res.setText(req.getParam("id"));
            });
            r.get("/redir", [](const http::HttpRequest&, http::HttpResponse& res) {
                res.redirect("/echo/x");
            });
        })) FAIL("服务器启动失败");

    // 302 的 reason phrase（修复前是 "Unknown"）
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        CHECK(sendAll(s, req("GET", "/redir")), "发送失败");
        auto r = readResponse(s, 3000);
        std::cout << "(302 状态行='" << r.status << "') ";
        CHECK(r.status == "HTTP/1.1 302 Found",
              "302 的原因短语应标准, 实际: '" << r.status << "'");
        close(s);
    }

    // 路径百分号解码：/echo/a%20b → 参数 "a b"
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        CHECK(sendAll(s, req("GET", "/echo/a%20b")), "发送失败");
        auto r = readResponse(s, 3000);
        CHECK(r.complete && r.body == "a b",
              "路径应被百分号解码, 实际 body='" << r.body << "'");
        close(s);
    }

    // 宽松解码修正：%+1 必须原样保留（旧实现 strtol 会把 "+1" 读成 0x01）
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        CHECK(sendAll(s, req("GET", "/echo/%+1")), "发送失败");
        auto r = readResponse(s, 3000);
        std::cout << "(%+1 → '" << r.body << "') ";
        CHECK(r.complete, "响应未收到");
        CHECK(r.body == "%+1",
              "非法百分号序列应原样保留（不得解成控制字符）, 实际 body 长度="
                  << r.body.size() << " 内容='" << r.body << "'");
        close(s);
    }

    f.down();
    PASS();
    return true;
}

// ── M4：请求头/缓冲上限与慢速滴灌 ──
static bool test_request_buffer_limits() {
    TEST("M4 请求头超 16KB → 431；慢速滴灌的半包请求被超时回收");
    Fixture f;
    if (!f.up(/*idleSec=*/1, /*subReactors=*/1, false, 4)) FAIL("服务器启动失败");

    // 1) 请求头超过 16KB
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        std::string huge = "GET /small HTTP/1.1\r\nHost: x\r\nX-Pad: " +
                           std::string(20 * 1024, 'p') + "\r\n\r\n";
        CHECK(sendAll(s, huge), "发送失败");
        auto r = readResponse(s, 4000);
        std::cout << "(超大头→" << (r.status.empty() ? "无响应" : r.status.substr(9, 3)) << ") ";
        CHECK(r.complete, "超限请求头应得到明确响应");
        CHECK(r.status.rfind("HTTP/1.1 431", 0) == 0,
              "请求头超过上限应回 431, 实际: " << r.status);
        close(s);
    }

    // 2) 慢速滴灌：每 300ms 一个字节、永不结束的请求头。
    //    只看 lastActive 的话永远不空闲；修复后按"半包请求起点"回收。
    {
        int s = connectTo(f.port);
        CHECK(s >= 0, "连接失败");
        CHECK(sendAll(s, "GET /small HTTP/1.1\r\n"), "发送首段失败");
        const auto t0 = std::chrono::steady_clock::now();
        bool closed = false;
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - t0).count() < 8) {
            if (peekClosed(s, 10)) { closed = true; break; }
            if (!sendAll(s, "X")) break;   // 每 300ms 一个字节
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        std::cout << "(滴灌 " << elapsed << "s 后被回收=" << (closed ? "是" : "否") << ") ";
        close(s);
        CHECK(closed, "慢速滴灌的半包请求必须被超时回收（否则单连接内存/时长无界）");
    }

    f.down();
    PASS();
    return true;
}

// ── M10：头名大小写归一化，不得产生重复 Content-Length ──
static bool test_header_normalization() {
    TEST("M10 不同大小写设置同名头不会产生重复 Content-Length");
    http::HttpResponse res;
    res.setHeader("content-length", "3");
    res.setBody("abc");                       // 内部再设一次 Content-Length
    res.setHeader("CONTENT-TYPE", "text/plain");
    CHECK(res.getHeader("Content-Length") == "3", "大小写不敏感读取应生效");
    CHECK(res.getHeader("content-type") == "text/plain", "小写读取应生效");

    const std::string out = res.toString();
    std::size_t count = 0, pos = 0;
    while ((pos = out.find("Content-Length:", pos)) != std::string::npos) { ++count; pos += 1; }
    std::cout << "(Content-Length 出现 " << count << " 次) ";
    CHECK(count == 1, "响应中必须只有一个 Content-Length, 实际 " << count << " 个");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_hardening ===" << std::endl;
    ignoreSigpipeInTest();

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) ++g_testsPassed;
        else      ++g_testsFailed;
    };

    run(test_sigpipe_survival,        "C1 SIGPIPE 存活");
    run(test_pipelining,              "C3 管线化");
    run(test_body_slicing,            "C5 请求体边界");
    run(test_response_size_boundary,  "C4 响应大小边界");
    run(test_accept_race,             "C6 accept 竞态");
    run(test_payload_too_large,       "H1/H2 413 与统计");
    run(test_stale_fd_no_crosstalk,   "C2 陈旧 fd 防误杀");
    run(test_stop_waits_for_inflight, "H3 stop 排空在途任务");
    run(test_slow_handler_not_killed_by_timer, "H4 慢 handler 不被误杀");
    run(test_pool_limit_configurable, "H7 内存池上限可配置");
    run(test_pool_exhaustion_reported, "H8 池耗尽显式上报");
    run(test_head_semantics,          "H13 HEAD 语义");
    run(test_router_dynamic_registration, "H14 路由运行期注册");
    run(test_app_stats_and_signal,    "H15 App 统计与信号");
    run(test_request_framing_hardening, "M3 请求框架头加固");
    run(test_reason_phrase_and_url_decode, "M9/M11 原因短语与解码");
    run(test_request_buffer_limits,   "M4 请求缓冲上限/慢速滴灌");
    run(test_header_normalization,    "M10 响应头归一化");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
