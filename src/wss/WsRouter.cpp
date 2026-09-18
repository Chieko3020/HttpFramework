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

// ── 路径计划：解析一次、缓存复用 ───────────────────────────────────

const WsRouter::PathPlan& WsRouter::planFor(const std::string& upgradePath) const {
    {
        std::lock_guard<std::mutex> lk(planMu_);
        auto it = planCache_.find(upgradePath);
        if (it != planCache_.end()) return it->second;
    }

    // 未命中：构建计划（只有"新路径的第一条消息"走这里）
    PathPlan plan;
    for (const auto& route : routes_) {
        if (route.handler && std::regex_match(upgradePath, route.pathRegex)) {
            plan.route = &route;
            plan.hasHandler = true;
            break;
        }
    }
    for (const auto& mw : globalMws_) plan.chain.push_back(&mw);
    for (const auto& mw : scopedMws_) {
        if (std::regex_match(upgradePath, mw.pathRegex)) plan.chain.push_back(&mw);
    }

    std::lock_guard<std::mutex> lk(planMu_);
    auto [it, inserted] = planCache_.emplace(upgradePath, std::move(plan));
    (void)inserted;
    return it->second;
}

// ── 分发 ───────────────────────────────────────────────────────────

void WsRouter::dispatch(const std::string& upgradePath, WssConnection& conn, WsMessage& msg) {
    const PathPlan& plan = planFor(upgradePath);
    if (plan.route) extractParams(*plan.route, upgradePath, conn);
    executeChain(plan, conn, msg);
}

// ── 中间件链执行 ──────────────────────────────────────────────────

// 实现说明：不使用 std::function 传递 next。
// 原来的写法每条消息都要构造 std::function 包装（递归 lambda + 每层一个 next
// 闭包），这里改成"按层展开的普通函数"：续行闭包只捕获一个 weak_ptr 与层号，
// 链的存活由 executeChain 持有的 shared_ptr 决定。语义不变：中间件同步调用
// next() 则按调用点之后的那一层继续。
namespace {

struct ChainRunner {
    const WsRouter::PathPlan& plan;
    WssConnection& conn;
    WsMessage& msg;

    void run(std::size_t index, const std::shared_ptr<ChainRunner>& keepAlive) {
        if (index >= plan.chain.size()) {
            if (plan.route && plan.route->handler) {
                plan.route->handler(conn, msg);
            } else {
                std::cerr << "[WARN][WSS路由]：无匹配handler, path="
                          << (plan.route ? plan.route->path : "(none)")
                          << " opcode=" << static_cast<int>(msg.opcode) << std::endl;
            }
            return;
        }
        std::weak_ptr<ChainRunner> weak = keepAlive;
        plan.chain[index]->middleware(conn, msg, [weak, index]() {
            if (auto sp = weak.lock()) sp->run(index + 1, sp);
        });
    }
};

}  // namespace

void WsRouter::executeChain(const PathPlan& plan, WssConnection& conn, WsMessage& msg) {
    if (plan.chain.empty()) {
        if (plan.route && plan.route->handler) plan.route->handler(conn, msg);
        else {
            std::cerr << "[WARN][WSS路由]：无匹配handler, path="
                      << (plan.route ? plan.route->path : "(none)")
                      << " opcode=" << static_cast<int>(msg.opcode) << std::endl;
        }
        return;
    }
    auto runner = std::make_shared<ChainRunner>(ChainRunner{plan, conn, msg});
    runner->run(0, runner);
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
