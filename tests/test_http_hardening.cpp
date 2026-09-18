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

// 按 Content-Length 读一个完整响应
static ParsedResponse readResponse(int sock, int timeoutMs = 5000) {
    ParsedResponse r;
    std::string buf;
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
    int port = 0;
    bool started = false;

    bool up(int idleSec = 60, size_t subReactors = 1, bool pool_ = false,
            size_t workers = 4) {
        port = pickPort();
        if (port == 0) return false;
        pool = std::make_unique<utils::ThreadPool>(workers);
        server = std::make_unique<http::HttpServer>(port, *pool, subReactors);
        server->setIdleTimeout(idleSec);
        if (pool_) server->enableMemoryPool(true);

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
        server->setRouter(r);

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

    for (int i = 0; i < 3; ++i) {
        auto r = readResponse(s);
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

        auto r1 = readResponse(s);
        CHECK(r1.complete, "POST 响应未收到");
        CHECK(r1.body == "hello",
              "echo 体应恰好是 Content-Length 指定的 5 字节, 实际: '" << r1.body << "'");

        auto r2 = readResponse(s);
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

            auto r = readResponse(s, 8000);
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
            auto r2 = readResponse(s);
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
static bool test_stale_fd_no_crosstalk() {
    TEST("C2 空闲超时关闭的连接，其 fd 号被复用后不被旧 worker 误杀");
    // 2 个 subReactor、idle=1s：慢请求在 reactor N，新连接轮询落在另一个 reactor
    Fixture f;
    if (!f.up(/*idleSec=*/1, /*subReactors=*/2, false, 4)) FAIL("服务器启动失败");

    // c1：6s 的慢请求（worker 在途），随后不再使用该连接，等空闲超时把它关掉
    int c1 = connectTo(f.port);
    CHECK(c1 >= 0, "c1 连接失败");
    CHECK(sendAll(c1, req("GET", "/slow")), "c1 发送失败");

    std::this_thread::sleep_for(std::chrono::seconds(5));

    // c2：复用刚归还的 fd 号
    int c2 = connectTo(f.port);
    CHECK(c2 >= 0, "c2 连接失败");
    CHECK(sendAll(c2, req("GET", "/small")), "c2 发送失败");
    auto r = readResponse(c2, 5000);
    CHECK(r.complete && r.body == "SMALL", "c2 首次响应异常: " << r.status);

    // 旧 worker 在 c1 连接后 6s 结束；期间 c2 必须一直可用
    int reused = 0;
    bool killed = false;
    for (int i = 0; i < 20; ++i) {
        if (peekClosed(c2)) { killed = true; break; }
        if (!sendAll(c2, req("GET", "/small"))) {
            killed = peekClosed(c2);
            break;
        }
        auto r2 = readResponse(c2, 2000);
        if (!(r2.complete && r2.body == "SMALL")) { killed = true; break; }
        ++reused;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "(复用成功 " << reused << " 次) ";

    close(c1);
    if (!killed) close(c2);
    else close(c2);

    CHECK(!killed,
          "c2 在旧 worker 完成时被单方面断开（陈旧 fd 误杀）");
    CHECK(reused >= 10, "复用次数过少: " << reused);

    f.down();
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

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
