// test_wss_hardening.cpp — WSS 深度审查修复的回归测试（仅 ENABLE_WSS）
//
// 覆盖（每条都对应一次已确认的缺陷）：
//   H9  0-RTT nonce 判据：非 0-RTT 连接不得因为环境变量而拒绝升级
//   H11 WsRouter::onClose 必须在断开路径上被调用（含关闭码）
//   H12 分片重组的累计长度必须有上限，超限以 1009 关闭
//   L9  升级请求校验（GET / Connection: Upgrade / Sec-WebSocket-Version: 13）
//   M18 升级头部长度上限
//
// 断言口径：客户端（TLS + WebSocket 握手）可观测行为。

#ifdef ENABLE_WSS

#include "HttpFramework/wss/WssReactor.h"
#include "utils/SocketCompat.h"   // net::ensureSigpipeIgnored：本进程同时充当客户端
#include "HttpFramework/wss/WsRouter.h"
#include "HttpFramework/wss/WebSocketCodec.h"
#include "utils/ThreadPool.h"

#include <openssl/ssl.h>
#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while (0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（计入 g_testsSkipped），不计入通过
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; return true; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;

// ───────────────────────── 证书 ─────────────────────────

static std::pair<std::string, std::string> generateCertKeyPair(const std::string& prefix) {
    std::string keyPath = "/tmp/" + prefix + "_key.pem";
    std::string certPath = "/tmp/" + prefix + "_cert.pem";
    std::string genCmd =
        "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath + " -out " + certPath +
        " -days 1 -nodes -subj '/CN=test.local' 2>/dev/null";
    int r = system(genCmd.c_str());
    (void)r;
    return {certPath, keyPath};
}

// ───────────────────────── TLS 客户端 ─────────────────────────

static int pickPort() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return 0; }
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &l);
    int port = ntohs(a.sin_port);
    close(s);
    return port;
}

struct TlsClient {
    int fd = -1;
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;
    std::string buf;   // 跨调用的残余字节

    bool connect(int port, int timeoutMs = 4000) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return false;
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(port);
        if (::connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) return false;
        ssl = SSL_new(ctx);
        if (!ssl) return false;
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) return false;
        return true;
    }

    bool writeAll(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            int n = SSL_write(ssl, data.data() + off, static_cast<int>(data.size() - off));
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    // 读取直到出现 "\r\n\r\n"；返回收到的原始字节（不含后续数据）
    std::string readHttpHeader(bool* gotHeader) {
        *gotHeader = false;
        while (true) {
            auto pos = buf.find("\r\n\r\n");
            if (pos != std::string::npos) {
                std::string head = buf.substr(0, pos + 4);
                buf.erase(0, pos + 4);
                *gotHeader = true;
                return head;
            }
            char tmp[4096];
            int n = SSL_read(ssl, tmp, sizeof(tmp));
            if (n <= 0) return buf;
            buf.append(tmp, static_cast<size_t>(n));
        }
    }

    void closeAll() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
        if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
    }
};

// ───────────────────────── WebSocket 帧 ─────────────────────────

static std::string wsUpgradeRequest(const std::string& path,
                                    const std::string& extraHeaders = "",
                                    const std::string& method = "GET") {
    // 固定 16 字节 key 的 base64
    return method + " " + path + " HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n" + extraHeaders + "\r\n";
}

// 构造客户端帧（可指定 fin 与 opcode），按 RFC 加掩码
static std::string buildClientFrameRaw(uint8_t opcode, const std::string& payload, bool fin) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));
    size_t len = payload.size();
    if (len <= 125) {
        out.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(mask[i]));
    for (size_t i = 0; i < payload.size(); ++i)
        out.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return out;
}

// 从一个已升级的连接上读一个服务端帧（服务端不加掩码）；超时返回 false
static bool readServerFrame(TlsClient& c, uint8_t* opcode, std::string* payload,
                            int timeoutMs = 3000) {
    http::wss::WebSocketStreamParser parser(false);
    parser.setOpenMode();
    std::string local = c.buf;   // 复制：解析器有状态，这里一次性解析足够
    c.buf.clear();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        std::string accept;
        std::vector<http::wss::WsFrame> frames;
        try {
            parser.feed(reinterpret_cast<const uint8_t*>(local.data()), local.size(), &accept,
                        &frames);
        } catch (...) {
            return false;
        }
        if (!frames.empty()) {
            *opcode = frames[0].opcode;
            payload->assign(frames[0].payload.begin(), frames[0].payload.end());
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        char tmp[8192];
        int n = SSL_read(c.ssl, tmp, sizeof(tmp));
        if (n > 0) {
            local.append(tmp, static_cast<size_t>(n));
            continue;
        }
        return false;
    }
}

// ───────────────────────── 服务器夹具 ─────────────────────────

struct WssFixture {
    std::unique_ptr<utils::ThreadPool> pool;
    std::unique_ptr<http::wss::WssReactor> reactor;
    std::shared_ptr<http::wss::WsRouter> router;
    int port = 0;
    std::atomic<int> opened{0};
    std::atomic<int> closed{0};
    std::atomic<int> closeCode{0};
    std::mutex mu;
    std::condition_variable cv;

    bool up(const std::string& cert, const std::string& key, const std::string& wsPath = "/echo",
            int idleSec = 30) {
        port = pickPort();
        if (port == 0) return false;
        pool = std::make_unique<utils::ThreadPool>(4);
        reactor = std::make_unique<http::wss::WssReactor>(static_cast<uint16_t>(port), cert, key,
                                                          *pool);
        router = std::make_shared<http::wss::WsRouter>();
        router->addHandler(wsPath, [](http::WssConnection& conn, const http::wss::WsMessage& msg) {
            if (msg.isText()) conn.sendText("Echo: " + msg.text());
        });
        router->setOpenHandler(wsPath, [this](http::WssConnection&) {
            opened.fetch_add(1);
        });
        router->setCloseHandler(wsPath, [this](http::WssConnection&, uint16_t code) {
            closeCode.store(code);
            closed.fetch_add(1);
            cv.notify_all();
        });
        reactor->setWsRouter(router);
        reactor->setWsIdleTimeout(idleSec);   // 生效于 start()（见 test_setter_takes_effect）
        if (!reactor->start()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    }

    // 等待 onClose 被调用（最多 ms 毫秒）
    bool waitClosed(int ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            return closed.load() > 0;
        });
    }

    void down() {
        if (reactor) reactor->stop();
        if (pool) pool->shutdown();
    }

    // 析构体里先 down()：onClose 回调会触碰 opened/closed/closeCode/mu/cv，
    // 若等成员按声明逆序析构（cv/mu 先走），关闭剩余连接时回调就会用到已析构对象
    ~WssFixture() { down(); }
};

// ───────────────────────── 用例 ─────────────────────────

// L9：升级请求校验（方法/Connection/Version）
static bool test_upgrade_validation(const std::string& cert, const std::string& key) {
    TEST("L9 升级请求校验：非 GET / 缺 Connection: Upgrade / Version!=13 一律拒绝");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    struct Case { const char* name; std::string req; bool expect101; };
    std::vector<Case> cases;
    cases.push_back({"正常 GET 升级", wsUpgradeRequest("/echo"), true});
    cases.push_back({"POST 升级", wsUpgradeRequest("/echo", "", "POST"), false});
    // 缺 Connection: Upgrade
    cases.push_back({"缺 Connection: Upgrade",
                     "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     false});
    // Version 不是 13
    cases.push_back({"Sec-WebSocket-Version: 8",
                     "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 8\r\n\r\n",
                     false});

    for (auto& cse : cases) {
        TlsClient c;
        if (!c.connect(f.port)) FAIL(std::string("TLS 连接失败: ") + cse.name);
        c.writeAll(cse.req);
        bool got = false;
        std::string head = c.readHttpHeader(&got);
        const bool is101 = got && head.rfind("HTTP/1.1 101", 0) == 0;
        std::cout << "[" << cse.name << "→" << (is101 ? "101" : "拒绝") << "] ";
        CHECK(is101 == cse.expect101,
              cse.name << " 期望 " << (cse.expect101 ? "101" : "拒绝")
                       << ", 实际: " << (got ? head.substr(0, 20) : std::string("对端直接断开")));
        c.closeAll();
    }

    f.down();
    PASS();
    return true;
}

// H9：环境变量不得再影响 nonce 校验（真实判据是 early data 是否被接受）
static bool test_nonce_not_env_driven(const std::string& cert, const std::string& key) {
    TEST("H9 设了 HTTPFW_WSS_ENABLE_0RTT 也不影响非 0-RTT 连接的升级");
    setenv("HTTPFW_WSS_ENABLE_0RTT", "1", 1);   // 只设环境变量，不开 TLS 0-RTT

    WssFixture f;
    if (!f.up(cert, key)) {
        unsetenv("HTTPFW_WSS_ENABLE_0RTT");
        FAIL("WSS 服务启动失败");
    }

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    c.closeAll();
    f.down();
    unsetenv("HTTPFW_WSS_ENABLE_0RTT");

    CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0,
          "0-RTT 未启用时不得要求 X-Nonce（修复前会因环境变量拒绝全部升级）: "
              << (got ? head.substr(0, 24) : std::string("对端直接断开")));
    PASS();
    return true;
}

// H11：onClose 必须被调用，并带上关闭码
static bool test_on_close_fired(const std::string& cert, const std::string& key) {
    TEST("H11 收到 close 帧后 onClose 被调用且 code=1000");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败: " << head.substr(0, 24));
    CHECK(f.opened.load() == 1, "onOpen 应被调用一次, 实际 " << f.opened.load());

    // 客户端主动发 close(1000)
    std::string closeFrame = buildClientFrameRaw(0x8, std::string("\x03\xe8", 2), true);
    CHECK(c.writeAll(closeFrame), "发送 close 帧失败");

    const bool fired = f.waitClosed(3000);
    std::cout << "(onClose 调用=" << f.closed.load() << " code=" << f.closeCode.load()
              << ") ";
    c.closeAll();
    f.down();

    CHECK(fired, "断开后 onClose 未被调用（修复前全仓无调用者，用户侧资源回收永不执行）");
    CHECK(f.closeCode.load() == 1000, "关闭码应为 1000, 实际 " << f.closeCode.load());
    PASS();
    return true;
}

// H11b：非正常断开（直接 RST/close）也要回调，code=1006
static bool test_on_close_abnormal(const std::string& cert, const std::string& key) {
    TEST("H11 客户端直接断开（不发 close 帧）时 onClose 仍被调用, code=1006");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    {
        TlsClient c;
        CHECK(c.connect(f.port), "TLS 连接失败");
        CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
        bool got = false;
        std::string head = c.readHttpHeader(&got);
        CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");
        // 直接关闭底层 fd（不发 close 帧）
        if (c.ssl) { SSL_free(c.ssl); c.ssl = nullptr; }
        if (c.fd >= 0) { ::close(c.fd); c.fd = -1; }
        if (c.ctx) { SSL_CTX_free(c.ctx); c.ctx = nullptr; }
    }

    const bool fired = f.waitClosed(4000);
    std::cout << "(onClose 调用=" << f.closed.load() << " code=" << f.closeCode.load() << ") ";
    f.down();
    CHECK(fired, "异常断开时 onClose 未被调用");
    CHECK(f.closeCode.load() == 1006, "异常断开的关闭码应为 1006, 实际 " << f.closeCode.load());
    PASS();
    return true;
}

// L9b：非法 UTF-8 文本帧必须以 1007 关闭；非最短长度编码必须以 1002 关闭
static bool test_l9_payload_checks(const std::string& cert, const std::string& key) {
    TEST("L9 非法 UTF-8 → 1007；非最短长度编码 → 1002");

    // 非法 UTF-8 文本帧
    {
        WssFixture f;
        if (!f.up(cert, key)) FAIL("WSS 服务启动失败");
        TlsClient c;
        CHECK(c.connect(f.port), "TLS 连接失败");
        CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
        bool got = false;
        std::string head = c.readHttpHeader(&got);
        CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");
        // 0xFF 不是合法的 UTF-8 起始字节
        CHECK(c.writeAll(buildClientFrameRaw(0x1, std::string("\xff\xfe", 2), true)),
              "发送非法 UTF-8 帧失败");
        uint8_t opcode = 0;
        std::string payload;
        const bool gotFrame = readServerFrame(c, &opcode, &payload, 3000);
        uint16_t code = 0;
        if (gotFrame && opcode == 0x8 && payload.size() >= 2)
            code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                         static_cast<uint8_t>(payload[1]));
        std::cout << "(utf8→opcode=0x" << std::hex << static_cast<int>(opcode) << std::dec
                  << " code=" << code << ") ";
        c.closeAll();
        f.down();
        CHECK(gotFrame && opcode == 0x8, "非法 UTF-8 应以 close 帧拒绝");
        CHECK(code == 1007, "非法 UTF-8 的关闭码应为 1007, 实际 " << code);
    }

    // 非最短长度编码：把 2 字节 payload 用 126 形式编码
    {
        WssFixture f;
        if (!f.up(cert, key)) FAIL("WSS 服务启动失败");
        TlsClient c;
        CHECK(c.connect(f.port), "TLS 连接失败");
        CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
        bool got = false;
        std::string head = c.readHttpHeader(&got);
        CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");

        const std::string payload2 = "hi";
        std::string frame;
        frame.push_back(static_cast<char>(0x81));            // FIN + text
        frame.push_back(static_cast<char>(0x80 | 126));      // masked + 2字节长度
        frame.push_back(0x00);
        frame.push_back(0x02);                               // 非最短编码
        unsigned char mask[4] = {1, 2, 3, 4};
        for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>(mask[i]));
        for (size_t i = 0; i < payload2.size(); ++i)
            frame.push_back(static_cast<char>(payload2[i] ^ mask[i % 4]));
        CHECK(c.writeAll(frame), "发送非最短编码帧失败");

        uint8_t opcode = 0;
        std::string payload;
        const bool gotFrame = readServerFrame(c, &opcode, &payload, 3000);
        uint16_t code = 0;
        if (gotFrame && opcode == 0x8 && payload.size() >= 2)
            code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                         static_cast<uint8_t>(payload[1]));
        std::cout << "(非最短→opcode=0x" << std::hex << static_cast<int>(opcode) << std::dec
                  << " code=" << code << ") ";
        c.closeAll();
        f.down();
        CHECK(gotFrame && opcode == 0x8, "非最短长度编码应以 close 帧拒绝");
        CHECK(code == 1002, "非最短长度编码的关闭码应为 1002, 实际 " << code);
    }

    PASS();
    return true;
}

// M18：升级头超过上限时拒绝（不再无界累积）
static bool test_upgrade_header_limit(const std::string& cert, const std::string& key) {
    TEST("M18 升级请求头超过 16KB 上限时拒绝");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    std::string req = wsUpgradeRequest("/echo", "X-Pad: " + std::string(32 * 1024, 'p') + "\r\n");
    CHECK(c.writeAll(req), "发送超大升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    const bool is101 = got && head.rfind("HTTP/1.1 101", 0) == 0;
    std::cout << "(超大头部→" << (is101 ? "101" : "拒绝") << ") ";
    c.closeAll();
    f.down();
    CHECK(!is101, "超过上限的升级请求不应被接受");
    PASS();
    return true;
}

// H12：分片累计超过上限必须以 1009 关闭
static bool test_fragment_limit(const std::string& cert, const std::string& key) {
    TEST("H12 分片累计超上限（2×40KB > 64KB）以 1009 关闭");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    CHECK(c.writeAll(wsUpgradeRequest("/echo")), "发送升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");

    // 每个分片 40KB < 单帧上限 64KB，但累计 80KB 超过消息上限
    const std::string frag(40 * 1024, 'A');
    CHECK(c.writeAll(buildClientFrameRaw(0x1, frag, /*fin=*/false)), "发送首个分片失败");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(c.writeAll(buildClientFrameRaw(0x0, frag, /*fin=*/false)), "发送续片失败");

    uint8_t opcode = 0;
    std::string payload;
    const bool gotFrame = readServerFrame(c, &opcode, &payload, 4000);
    uint16_t code = 0;
    if (gotFrame && opcode == 0x8 && payload.size() >= 2)
        code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                     static_cast<uint8_t>(payload[1]));
    std::cout << "(opcode=0x" << std::hex << static_cast<int>(opcode) << std::dec
              << " closeCode=" << code << ") ";
    c.closeAll();

    if (gotFrame && opcode == 0x1) {
        // 服务端把 80KB 当成一条消息回显了：说明没有累计上限
        FAIL("服务端接受了超过上限的分片消息（无累计上限）");
    }
    f.down();
    CHECK(gotFrame, "超限分片后应收到关闭帧（修复前会一直挂到空闲超时）");
    CHECK(opcode == 0x8, "应收到 close 帧, 实际 opcode=0x" << static_cast<int>(opcode));
    CHECK(code == 1009, "关闭码应为 1009(message too big), 实际 " << code);
    PASS();
    return true;
}

// ③ flushOutbound 改成"锁内搬运、锁外 SSL_write"后的正确性回归。
//
// 可判别性设计：
//   1) handler 在同一把锁内完成"取号 + sendText"，于是序号就是入队顺序；
//   2) 客户端连发 2 条请求后**停止读取**：服务端每条回 60KB（远大于 4KB
//      SO_SNDBUF），于是出站队列堆到 2 项，并卡在第一项的中途（WANT_WRITE）；
//   3) 客户端再用 512B/1ms 的小步长慢慢排空，期间服务端会反复进入部分写；
//      如果"队列顺序"被破坏（未发完的队首先行、新数据插到队首之前），
//      到达顺序就会跟着改变；
//   4) 按到达顺序解析帧，要求恰好是 seq-0、seq-1（序号即入队顺序）。
// 注入故障验证（见简报）：WANT_WRITE 分支里 pop_front 丢弃未发完的项 → 该用例失败。
static bool test_outbound_order_under_concurrency(const std::string& cert, const std::string& key) {
    TEST("③ 队列非空且发生部分写时，出站帧顺序 == 入队顺序");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");
    // 4KB 发送缓冲：60KB 的帧必然"只写出一部分"，队列会堆起来
    f.reactor->setSocketSendBuffer(4096);

    const std::string big(60 * 1024, 'x');
    std::mutex seqMu;
    uint64_t seqNext = 0;
    f.router->addHandler("/seq", [&seqMu, &seqNext, big](http::WssConnection& conn,
                                                         const http::wss::WsMessage& msg) {
        if (!msg.isText()) return;
        // 取号与入队在同一把锁内，因此序号严格等于入队顺序
        std::lock_guard<std::mutex> lk(seqMu);
        uint64_t n = seqNext++;
        conn.sendText("seq-" + std::to_string(n) + ":" + big);
    });

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    CHECK(c.writeAll(wsUpgradeRequest("/seq")), "发送升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");

    CHECK(c.writeAll(buildClientFrameRaw(0x1, "m", true)), "第 1 条消息发送失败");
    CHECK(c.writeAll(buildClientFrameRaw(0x1, "m", true)), "第 2 条消息发送失败");
    // 不读取：让服务端把两条回显都排进队列，并卡在第一项中途
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    http::wss::WebSocketStreamParser parser(false);
    parser.setOpenMode();
    std::string local = c.buf;
    c.buf.clear();
    std::vector<uint64_t> gotSeq;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (gotSeq.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::string accept;
        std::vector<http::wss::WsFrame> frames;
        try {
            parser.feed(reinterpret_cast<const uint8_t*>(local.data()), local.size(), &accept, &frames);
        } catch (...) { break; }
        local.clear();
        for (const auto& fr : frames) {
            if (fr.opcode != 0x1) continue;
            std::string text(fr.payload.begin(), fr.payload.end());
            if (text.rfind("seq-", 0) != 0) { gotSeq.push_back(9999); continue; }
            auto colon = text.find(':');
            gotSeq.push_back(std::stoull(text.substr(4, colon - 4)));
        }
        if (gotSeq.size() >= 2) break;
        // 小步长慢读：让服务端反复进入"部分写"分支
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        char tmp[512];
        int n = SSL_read(c.ssl, tmp, sizeof(tmp));
        if (n <= 0) break;
        local.append(tmp, static_cast<size_t>(n));
    }
    std::cout << "(序号帧=" << gotSeq.size() << ") ";
    c.closeAll();
    f.down();

    CHECK(gotSeq.size() == 2, "应恰好收到 2 条序号帧, 实际 " << gotSeq.size());
    CHECK(gotSeq.size() == 2 && gotSeq[0] == 0 && gotSeq[1] == 1,
          "序号帧顺序应为 0,1");
    PASS();
    return true;
}

// ③ WsRouter 路径计划缓存：同一路径的第二条消息必须用缓存仍得到与第一条
// 完全相同的结果（路径参数、中间件链、handler 全部命中）。
static bool test_router_plan_cache(const std::string& cert, const std::string& key) {
    TEST("③ WsRouter 计划缓存：参数路由 + 全局/作用域中间件在多条消息上一致");
    WssFixture f;
    if (!f.up(cert, key)) FAIL("WSS 服务启动失败");

    std::atomic<int> globalCount{0};
    std::atomic<int> scopedCount{0};
    f.router->addMiddleware([&globalCount](http::WssConnection&, http::wss::WsMessage&,
                                          std::function<void()> next) {
        globalCount.fetch_add(1);
        next();
    });
    // 注意：作用域中间件是"精确路径匹配"（compilePath 生成 ^/users/42$），
    // 不像 HTTP 侧的 RouterHandler 那样做前缀/参数匹配 —— 这里按实现语义注册
    f.router->addMiddleware("/users/42", [&scopedCount](http::WssConnection&, http::wss::WsMessage&,
                                                       std::function<void()> next) {
        scopedCount.fetch_add(1);
        next();
    });
    // 参数路由：handler 把 :id 回显出来，验证缓存命中时参数仍然提取正确
    f.router->addHandler("/users/:id", [](http::WssConnection& conn,
                                          const http::wss::WsMessage& msg) {
        if (msg.isText()) conn.sendText("id=" + conn.getUserData("ws_param_id"));
    });

    TlsClient c;
    CHECK(c.connect(f.port), "TLS 连接失败");
    CHECK(c.writeAll(wsUpgradeRequest("/users/42")), "发送升级请求失败");
    bool got = false;
    std::string head = c.readHttpHeader(&got);
    CHECK(got && head.rfind("HTTP/1.1 101", 0) == 0, "升级失败");

    std::vector<std::string> replies;
    for (int i = 0; i < 3; ++i) {
        CHECK(c.writeAll(buildClientFrameRaw(0x1, "ping", true)), "发送消息失败");
        uint8_t opcode = 0;
        std::string payload;
        if (!readServerFrame(c, &opcode, &payload, 3000)) break;
        if (opcode == 0x1) replies.push_back(payload);
    }
    std::cout << "(回复数=" << replies.size() << " 全局mw=" << globalCount.load()
              << " 作用域mw=" << scopedCount.load() << ") ";
    c.closeAll();
    f.down();

    CHECK(replies.size() == 3, "应收到 3 条回复, 实际 " << replies.size());
    for (const auto& r : replies) CHECK(r == "id=42", "参数应始终为 42, 实际 " << r);
    CHECK(globalCount.load() == 3, "全局中间件应每条消息执行一次, 实际 " << globalCount.load());
    CHECK(scopedCount.load() == 3, "作用域中间件应每条消息执行一次, 实际 " << scopedCount.load());
    PASS();
    return true;
}

// ④ 半开连接必须被空闲超时回收，且 setWsIdleTimeout() 必须真的生效。
//
// 两个已确认缺陷的判别用例：
//   a) 心跳与空闲回收曾被合并进只遍历 activeFds（= 已握手连接）的循环，
//      因此"只建 TCP、一个字节都不发"的连接既不进 activeFds、也不被任何回收
//      路径触及 → 永远驻留（fd + SSL* 泄漏，slowloris 型耗尽）。
//   b) setWsIdleTimeout() 只写 st.idleTimeoutSec，而 start() 用私有成员把它
//      覆盖回默认 120s ⇒ setter 完全无效，WSS 超时相关用例全部假绿。
// 本用例从客户端侧观测：设 idle=5s，建裸 TCP 连接，8s 内必须收到 EOF。
static bool test_idle_reaps_unhandshaked_conn(const std::string& cert, const std::string& key) {
    TEST("④ 未握手连接被空闲超时回收（setWsIdleTimeout 生效）");
    WssFixture f;
    if (!f.up(cert, key, "/echo", /*idleSec=*/5)) FAIL("WSS 服务启动失败");

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "创建 TCP socket 失败");
    struct timeval tv;
    tv.tv_sec = 1; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(static_cast<uint16_t>(f.port));
    CHECK(::connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0, "TCP 连接失败");

    // 一个字节都不发：服务端既没完成 TLS 握手，也没升级。
    // idle=5s（setter 下限）、定时器 1s 一轮 ⇒ 判定条件 now-last_ping > 5s，
    // 因此最晚 t≈6s 应收到 EOF；给到 8s 的余量。旧实现恒不回收。
    bool eof = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[64];
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) { eof = true; break; }                       // 对端 close → EOF
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { eof = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    f.down();

    CHECK(eof, "半开连接应在 idle 超时后被回收（4s 内收到 EOF），实际一直保持");
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_wss_hardening ===" << std::endl;

    // 本进程既是服务端又是客户端：客户端向"已被服务端关闭的连接"写入会收到
    // SIGPIPE（默认动作是终止进程）。HTTP 侧由 HttpServer 构造时忽略，WSS 侧
    // 由测试自己兜底。
    http::net::ensureSigpipeIgnored();

    auto [cert, key] = generateCertKeyPair("wssh");

    auto run = [](bool (*fn)(const std::string&, const std::string&), const char* name,
                  const std::string& c, const std::string& k) {
        std::cout << "[运行] " << name << std::endl;
        if (fn(c, k)) ++g_testsPassed;
        else ++g_testsFailed;
    };

    run(test_upgrade_validation,                 "L9 升级请求校验", cert, key);
    run(test_nonce_not_env_driven,               "H9 nonce 判据", cert, key);
    run(test_on_close_fired,                     "H11 onClose（正常关闭）", cert, key);
    run(test_on_close_abnormal,                  "H11 onClose（异常断开）", cert, key);
    run(test_fragment_limit,                     "H12 分片累计上限", cert, key);
    run(test_l9_payload_checks,                  "L9 UTF-8/长度编码", cert, key);
    run(test_upgrade_header_limit,               "M18 升级头上限", cert, key);
    run(test_outbound_order_under_concurrency,   "③ 并发出站顺序", cert, key);
    run(test_router_plan_cache,                  "③ 路由计划缓存", cert, key);
    run(test_idle_reaps_unhandshaked_conn,       "④ 半开连接空闲回收", cert, key);

    { int rc = system(("rm -f " + cert + " " + key).c_str()); (void)rc; }

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, " << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过" << std::endl;
    return g_testsFailed > 0 ? 1 : 0;
}

#endif  // ENABLE_WSS
