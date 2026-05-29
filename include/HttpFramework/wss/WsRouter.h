#pragma once

// WS 路由器 — 路径匹配 + 中间件链
// API 风格对齐 HttpFramework 的 router::Router

#include "WssTypes.h"

#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace http {
namespace wss {

class WsRouter {
public:
    WsRouter() = default;
    ~WsRouter() = default;

    // ── 消息处理器 ──
    void addHandler(const std::string& path, WsHandler handler);

    // ── 中间件 ──
    void addMiddleware(WsMiddleware mw);
    void addMiddleware(const std::string& path, WsMiddleware mw);

    // ── 连接生命周期回调 ──
    void setOpenHandler(const std::string& path, WsOpenHandler h);
    void setCloseHandler(const std::string& path, WsCloseHandler h);

    // ── 分发（由 WssReactor 在线程池中调用）──
    void dispatch(const std::string& upgradePath, WssConnection& conn, WsMessage& msg);

    // ── 生命周期通知（由 WssReactor 在 IO 线程调用）──
    void onOpen(const std::string& upgradePath, WssConnection& conn);
    void onClose(const std::string& upgradePath, WssConnection& conn, uint16_t code);

private:
    struct RouteEntry {
        std::string path;
        std::regex pathRegex;
        std::vector<std::string> paramNames;
        WsHandler handler;
        WsOpenHandler openHandler;
        WsCloseHandler closeHandler;
    };

    struct MwEntry {
        std::string path;       // 空 = 全局
        std::regex pathRegex;   // 空 path 的 regex 匹配任意
        WsMiddleware middleware;
    };

    std::vector<MwEntry> globalMws_;
    std::vector<MwEntry> scopedMws_;
    std::vector<RouteEntry> routes_;

    // 将 /users/:id 转为 ^/users/([^/]+)$
    static std::pair<std::regex, std::vector<std::string>> compilePath(const std::string& path);

    // 从请求路径提取路由参数
    static void extractParams(const RouteEntry& route, const std::string& requestPath,
                              WssConnection& conn);

    // 收集匹配的中间件并执行链
    void executeChain(WssConnection& conn, WsMessage& msg,
                      const std::function<void()>& finalHandler);
};

}  // namespace wss
}  // namespace http
