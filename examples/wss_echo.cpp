// WSS Echo 示例 — 需要 ENABLE_WSS=ON
// 用法: ./wss_echo
// 然后浏览器连接 wss://localhost:9443/echo 发送文本

#include "HttpFramework.h"
#include <iostream>

int main() {
    http::App app;

    // ── HTTP 路由 ──
    app.get("/", [](const auto& /*req*/, auto& res) {
        res.setHtml(
            "<h1>HttpFramework + WSS</h1>"
            "<p>HTTP server running on port 8080</p>"
            "<p>Connect to <code>wss://localhost:9443/echo</code> for WebSocket echo</p>"
        );
    });

    // ── WSS 路由 ──
    app.enableWss(9443, "certs/server_cert.pem", "certs/server_key.pem");

    // Echo 端点 — 匹配任意路径，收到什么文本就回什么
    app.ws("/", [](http::WssConnection& conn, const http::wss::WsMessage& msg) {
        if (msg.isText()) {
            conn.sendText("Echo: " + msg.text());
        }
    });

    // 连接/断开日志
    app.onWsOpen("/", [](http::WssConnection& conn) {
        std::cout << "[INFO][WSS]：客户端已连接, id=" << conn.id()
                  << " from=" << conn.remoteAddr() << std::endl;
    });

    app.onWsClose("/", [](http::WssConnection& conn, uint16_t code) {
        std::cout << "[INFO][WSS]：客户端已断开, id=" << conn.id()
                  << " code=" << code << std::endl;
    });

    std::cout << "[INFO][HTTP框架]：启动 HttpFramework + WSS ..." << std::endl;
    app.start(8080, 4);
}
