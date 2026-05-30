// bench_server.cpp — 性能基准测试服务器
// 用途: 配合 wrk/curl 等工具对框架各模块进行独立性能测定
// 用法: bench_server [选项]
//   --port PORT        HTTP 端口 (默认 8080)
//   --threads N        工作线程数 (默认 4)
//   --mempool          启用内存池
//   --routes N         额外注册 N 条路由 (测路由扩展性, 默认 0)
//   --middleware N     额外全局中间件层数 (默认 0)
//   --session          启用 Session 中间件
//   --template         启用 Template 端点 (测文件 I/O)
//   --wss-port PORT    WSS 端口 (需 ENABLE_WSS, 默认 0=禁用)
//   --cert FILE        TLS 证书路径 (默认 /tmp/bench_cert.pem)
//   --key FILE         TLS 私钥路径 (默认 /tmp/bench_key.pem)
//   --help             显示帮助

#include "HttpFramework.h"
#include "utils/TemplateLoader.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <string>

// ── 基准端点 handler ──────────────────────────────────────────

// A1: 纯文本响应 — 测量原始吞吐量上限
static void handlePlaintext(const http::HttpRequest&, http::HttpResponse& res) {
    res.setText("Hello, World!");
}

// A2: JSON 响应 — 模拟 API 场景
static void handleJson(const http::HttpRequest&, http::HttpResponse& res) {
    res.setJson(R"({"message":"Hello, World!"})");
}

// B3: 动态路由 — 参数提取 + JSON
static void handleUser(const http::HttpRequest& req, http::HttpResponse& res) {
    res.setJson(R"({"id":")" + req.getParam("id") + R"("})");
}

// POST echo — 回显请求体
static void handleEcho(const http::HttpRequest& req, http::HttpResponse& res) {
    res.setText(req.getBody());
}

#ifdef ENABLE_WSS
// WSS echo — WebSocket 回显
static void handleWsEcho(http::WssConnection& conn, const http::wss::WsMessage& msg) {
    if (msg.isText()) {
        conn.sendText(msg.text());
    } else if (msg.isBinary()) {
        conn.sendBinary(msg.payload);
    }
}
#endif

// ── 帮助 ────────────────────────────────────────────────────────

static void printHelp(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n\n"
              << "选项:\n"
              << "  --port PORT        HTTP 端口 (默认 8080)\n"
              << "  --threads N        工作线程数 (默认 4)\n"
              << "  --mempool          启用内存池 (12KB 块, 5000 预分配)\n"
              << "  --routes N         额外注册 N 条路由 (默认 0, 最大 5000)\n"
              << "  --middleware N     额外全局中间件层数 (默认 0, 最大 50)\n"
              << "  --session          启用 Session 中间件\n"
              << "  --template         启用 Template 端点\n"
#ifdef ENABLE_WSS
              << "  --wss-port PORT    WSS 端口 (默认 0=禁用)\n"
              << "  --cert FILE        TLS 证书路径 (默认 /tmp/bench_cert.pem)\n"
              << "  --key FILE         TLS 私钥路径 (默认 /tmp/bench_key.pem)\n"
#endif
              << "  --help             显示此帮助\n"
              << "\n基准端点 (始终注册):\n"
              << "  GET  /bench/plaintext   纯文本 \"Hello, World!\"\n"
              << "  GET  /bench/json         JSON 响应\n"
              << "  GET  /bench/user/:id     动态路由 + JSON\n"
              << "  POST /bench/echo          回显请求体\n"
              << "  GET  /stats              框架内部统计 (JSON)\n";
}

// ── main ─────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // 默认值
    int port = 8080;
    int threads = 4;
    bool useMempool = false;
    int extraRoutes = 0;
    int extraMiddleware = 0;
    bool useSession = false;
    bool useTemplate = false;
#ifdef ENABLE_WSS
    int wssPort = 0;
    std::string certFile = "/tmp/bench_cert.pem";
    std::string keyFile = "/tmp/bench_key.pem";
#endif

    // 解析 CLI
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg == "--mempool") {
            useMempool = true;
        } else if (arg == "--routes" && i + 1 < argc) {
            extraRoutes = std::min(std::stoi(argv[++i]), 5000);
        } else if (arg == "--middleware" && i + 1 < argc) {
            extraMiddleware = std::min(std::stoi(argv[++i]), 50);
        } else if (arg == "--session") {
            useSession = true;
        } else if (arg == "--template") {
            useTemplate = true;
#ifdef ENABLE_WSS
        } else if (arg == "--wss-port" && i + 1 < argc) {
            wssPort = std::stoi(argv[++i]);
        } else if (arg == "--cert" && i + 1 < argc) {
            certFile = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            keyFile = argv[++i];
#endif
        } else {
            std::cerr << "未知选项: " << arg << " (--help 查看帮助)\n";
            return 1;
        }
    }

    http::App app;

    // ── 可选功能 ──────────────────────────────────────────

    if (useMempool) {
        app.enableMemoryPool(true);
    }

    if (useSession) {
        app.enableSession();
    }

    // ── 固定基准端点 ──────────────────────────────────────

    app.get("/bench/plaintext", handlePlaintext);
    app.get("/bench/json", handleJson);
    app.get("/bench/user/:id", handleUser);
    app.post("/bench/echo", handleEcho);

    // ── 健康检查 ──────────────────────────────────────────

    app.get("/stats", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setJson(R"({"status":"ok"})");
    });

    // ── 额外路由 (B3: 路由扩展性) ─────────────────────────

    for (int i = 0; i < extraRoutes; ++i) {
        std::string path = "/bench/routes/" + std::to_string(i);
        app.get(path, [i](const http::HttpRequest&, http::HttpResponse& res) {
            res.setJson(R"({"route":)" + std::to_string(i) + "}");
        });
    }

    // ── 额外中间件 (C1: 中间件开销) ───────────────────────

    for (int i = 0; i < extraMiddleware; ++i) {
        app.use([i](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
            (void)i;
            next();
        });
    }

    // ── Template 端点 (如果启用) ──────────────────────────

    if (useTemplate) {
        // 检查模板目录是否可访问
        std::ifstream testFile("templates/index.html");
        if (!testFile.good()) {
            std::cerr << "[BENCH] 警告: templates/index.html 不可访问, "
                      << "请在 build 目录下运行 bench_server\n";
        }
        app.get("/bench/template", [](const http::HttpRequest&, http::HttpResponse& res) {
            std::string rendered = utils::TemplateLoader::loadTemplate("index.html");
            if (rendered.empty()) {
                res.setStatus(http::HttpStatus::NOT_FOUND);
                res.setJson(R"({"error":"template not found"})");
            } else {
                res.setHtml(rendered);
            }
        });
    }

    // ── Session 端点 (如果启用) ───────────────────────────

    if (useSession) {
        app.get("/bench/session", [](const http::HttpRequest& req, http::HttpResponse& res) {
            std::string sid = req.getUserData("session");
            res.setJson(R"({"session":")" + sid + R"("})");
        });
    }

    // ── WSS (如果启用) ────────────────────────────────────

#ifdef ENABLE_WSS
    if (wssPort > 0) {
        app.enableWss(static_cast<uint16_t>(wssPort), certFile, keyFile);
        app.ws("/echo", handleWsEcho);
    }
#endif

    // ── 启动信息 ──────────────────────────────────────────

    std::cout << "[BENCH] 配置:\n"
              << "  HTTP 端口:    " << port << "\n"
              << "  工作线程:     " << threads << "\n"
              << "  内存池:       " << (useMempool ? "启用" : "禁用") << "\n"
              << "  额外路由:     " << extraRoutes << "\n"
              << "  额外中间件:   " << extraMiddleware << "\n"
              << "  Session:      " << (useSession ? "启用" : "禁用") << "\n"
              << "  Template:     " << (useTemplate ? "启用" : "禁用") << "\n";
#ifdef ENABLE_WSS
    if (wssPort > 0) {
        std::cout << "  WSS 端口:     " << wssPort << "\n"
                  << "  TLS 证书:     " << certFile << "\n"
                  << "  TLS 私钥:     " << keyFile << "\n";
    }
#endif

    std::cout << "\n[BENCH] 可用端点:\n"
              << "  GET  /bench/plaintext\n"
              << "  GET  /bench/json\n"
              << "  GET  /bench/user/:id\n"
              << "  POST /bench/echo\n"
              << "  GET  /stats\n";
    if (extraRoutes > 0)
        std::cout << "  GET  /bench/routes/0 .. /bench/routes/" << extraRoutes - 1 << "\n";
    if (useTemplate)
        std::cout << "  GET  /bench/template\n";
    if (useSession)
        std::cout << "  GET  /bench/session\n";
#ifdef ENABLE_WSS
    if (wssPort > 0)
        std::cout << "  WSS  /echo\n";
#endif
    std::cout << std::endl;

    app.start(port, threads);
    return 0;
}
