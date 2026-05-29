// WS 路由器实现

#include "HttpFramework/wss/WsRouter.h"
#include "HttpFramework/wss/WssConnection.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace http {
namespace wss {

// ── 路径编译：/users/:id → regex ^/users/([^/]+)$ ──────────────────

std::pair<std::regex, std::vector<std::string>> WsRouter::compilePath(const std::string& path) {
    std::vector<std::string> paramNames;
    std::ostringstream oss;
    oss << "^";

    std::istringstream iss(path);
    std::string segment;
    while (std::getline(iss, segment, '/')) {
        if (segment.empty()) continue;
        oss << "/";
        if (segment.front() == ':' && segment.size() > 1) {
            paramNames.push_back(segment.substr(1));
            oss << "([^/]+)";
        } else if (segment == "*") {
            oss << ".*";
        } else {
            // 转义正则特殊字符
            for (char ch : segment) {
                if (ch == '.' || ch == '+' || ch == '?' || ch == '(' || ch == ')' ||
                    ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '\\')
                    oss << '\\';
                oss << ch;
            }
        }
    }

    if (path == "/") oss << "/";
    oss << "$";

    return {std::regex(oss.str(), std::regex::optimize), paramNames};
}

// ── 参数提取 ───────────────────────────────────────────────────────

void WsRouter::extractParams(const RouteEntry& route, const std::string& requestPath,
                              WssConnection& conn) {
    std::smatch match;
    if (std::regex_match(requestPath, match, route.pathRegex)) {
        for (std::size_t i = 0; i < route.paramNames.size() && (i + 1) < match.size(); ++i) {
            conn.setUserData("ws_param_" + route.paramNames[i], match[i + 1].str());
        }
    }
}

// ── 注册 ───────────────────────────────────────────────────────────

void WsRouter::addHandler(const std::string& path, WsHandler handler) {
    auto [regex, paramNames] = compilePath(path);
    RouteEntry entry;
    entry.path = path;
    entry.pathRegex = std::move(regex);
    entry.paramNames = std::move(paramNames);
    entry.handler = std::move(handler);
    routes_.push_back(std::move(entry));
}

void WsRouter::addMiddleware(WsMiddleware mw) {
    MwEntry entry;
    entry.middleware = std::move(mw);
    globalMws_.push_back(std::move(entry));
}

void WsRouter::addMiddleware(const std::string& path, WsMiddleware mw) {
    auto [regex, _] = compilePath(path);
    MwEntry entry;
    entry.path = path;
    entry.pathRegex = std::move(regex);
    entry.middleware = std::move(mw);
    scopedMws_.push_back(std::move(entry));
}

void WsRouter::setOpenHandler(const std::string& path, WsOpenHandler h) {
    auto [regex, paramNames] = compilePath(path);
    // 找已有路由或新建
    for (auto& r : routes_) {
        if (r.path == path) { r.openHandler = std::move(h); return; }
    }
    RouteEntry entry;
    entry.path = path;
    entry.pathRegex = std::move(regex);
    entry.paramNames = std::move(paramNames);
    entry.openHandler = std::move(h);
    routes_.push_back(std::move(entry));
}

void WsRouter::setCloseHandler(const std::string& path, WsCloseHandler h) {
    for (auto& r : routes_) {
        if (r.path == path) { r.closeHandler = std::move(h); return; }
    }
    auto [regex, paramNames] = compilePath(path);
    RouteEntry entry;
    entry.path = path;
    entry.pathRegex = std::move(regex);
    entry.paramNames = std::move(paramNames);
    entry.closeHandler = std::move(h);
    routes_.push_back(std::move(entry));
}

// ── 分发 ───────────────────────────────────────────────────────────

void WsRouter::dispatch(const std::string& upgradePath, WssConnection& conn, WsMessage& msg) {
    // 查找匹配路由
    RouteEntry* matchedRoute = nullptr;
    for (auto& route : routes_) {
        if (route.handler && std::regex_match(upgradePath, route.pathRegex)) {
            matchedRoute = &route;
            extractParams(route, upgradePath, conn);
            break;
        }
    }

    // 构建最终 handler（路由 handler 或 fallback）
    auto finalHandler = [matchedRoute, &conn, &msg]() {
        if (matchedRoute && matchedRoute->handler) {
            matchedRoute->handler(conn, msg);
        } else {
            std::cerr << "[WARN][WSS路由]：无匹配handler, path="
                      << (matchedRoute ? matchedRoute->path : "(none)")
                      << " opcode=" << static_cast<int>(msg.opcode) << std::endl;
        }
    };

    executeChain(conn, msg, finalHandler);
}

// ── 中间件链执行 ──────────────────────────────────────────────────

void WsRouter::executeChain(WssConnection& conn, WsMessage& msg,
                             const std::function<void()>& finalHandler) {
    // 收集匹配的中间件：全局 + 路经匹配的作用域中间件
    struct MwRef {
        WsMiddleware* fn;
    };
    std::vector<MwRef> chain;

    // 全局中间件
    for (auto& mw : globalMws_)
        chain.push_back({&mw.middleware});

    // 作用域中间件（匹配路径的）
    for (auto& mw : scopedMws_) {
        if (std::regex_match(conn.state.upgradePath, mw.pathRegex))
            chain.push_back({&mw.middleware});
    }

    if (chain.empty()) {
        finalHandler();
        return;
    }

    // 递归执行链
    std::function<void(std::size_t)> run;
    run = [&run, &chain, &conn, &msg, &finalHandler](std::size_t index) {
        if (index >= chain.size()) {
            finalHandler();
            return;
        }
        auto next = [&run, index]() { run(index + 1); };
        (*chain[index].fn)(conn, msg, next);
    };
    run(0);
}

// ── 生命周期通知 ──────────────────────────────────────────────────

void WsRouter::onOpen(const std::string& upgradePath, WssConnection& conn) {
    for (auto& route : routes_) {
        if (route.openHandler && std::regex_match(upgradePath, route.pathRegex)) {
            extractParams(route, upgradePath, conn);
            
            route.openHandler(conn);
            return;
        }
    }
}

void WsRouter::onClose(const std::string& upgradePath, WssConnection& conn, uint16_t code) {
    for (auto& route : routes_) {
        if (route.closeHandler && std::regex_match(upgradePath, route.pathRegex)) {
            route.closeHandler(conn, code);
            return;
        }
    }
}

}  // namespace wss
}  // namespace http
