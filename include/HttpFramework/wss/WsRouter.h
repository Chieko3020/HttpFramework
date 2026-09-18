#pragma once

// WS 路由器 — 路径匹配 + 中间件链
// API 风格对齐 HttpFramework 的 router::Router

#include "WssTypes.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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

    // ── 分发（由 WssReactor 在线程池中调用）──
    void dispatch(const std::string& upgradePath, WssConnection& conn, WsMessage& msg);

    // 一个升级路径的解析结果（缓存项）。dispatch 是消息热路径：原来每条消息
    // 都要遍历全部路由做 regex_match、并现构造 std::function 链；现在同一路径
    // 只解析一次，之后每次消息只是取一份指针列表。
    struct PathPlan {
        const RouteEntry* route{nullptr};
        std::vector<const MwEntry*> chain;   // 全局中间件 + 该路径匹配的作用域中间件
    };

    // ── 生命周期通知（由 WssReactor 在 **I/O 线程** 调用）──
    // 注意：onOpen/onClose 都在 I/O 线程执行，回调里不要做阻塞操作；
    // code 是本次连接的关闭码（正常 1000 / going away 1001 / 异常 1006 /
    // 协议错误 1002 / 消息过大 1009）。onClose 在 close(fd) 之前调用，
    // 此时连接对象仍然有效（conn.isOpen() 已为 false）。
    void onOpen(const std::string& upgradePath, WssConnection& conn);
    void onClose(const std::string& upgradePath, WssConnection& conn, uint16_t code);

private:
    // 注册表用 std::deque 而不是 std::vector：planCache_ 里的 PathPlan 持有
    // 元素的指针，deque 在两端插入时**不搬移已存在的元素**（引用/指针保持有效），
    // 因此缓存建立后再 addHandler/addMiddleware 也不会让缓存里的指针悬空。
    // 若改回 vector，扩容会让缓存里的指针全部失效（heap-use-after-free）。
    std::deque<MwEntry> globalMws_;
    std::deque<MwEntry> scopedMws_;
    std::deque<RouteEntry> routes_;

    // upgradePath → PathPlan（惰性填充）。注册（addHandler/addMiddleware/
    // setOpenHandler/setCloseHandler）会清空这份缓存，避免"注册前的未命中
    // 结论"把新注册的路由/中间件永久屏蔽（否定结论不能长期缓存）。
    mutable std::mutex planMu_;
    mutable std::unordered_map<std::string, std::shared_ptr<const PathPlan>> planCache_;

    // 注册/注销后使缓存失效（调用方需持有 planMu_）
    void invalidatePlanCacheLocked() { planCache_.clear(); }

    // 解析（或取缓存的）路径计划。返回 shared_ptr：调用方在锁外使用这份计划，
    // 期间可能有其他线程注册路由并触发 planCache_ 重哈希，值语义才能真正保活。
    std::shared_ptr<const PathPlan> planFor(const std::string& upgradePath) const;

    // 将 /users/:id 转为 ^/users/([^/]+)$
    static std::pair<std::regex, std::vector<std::string>> compilePath(const std::string& path);

    // 从请求路径提取路由参数
    static void extractParams(const RouteEntry& route, const std::string& requestPath,
                              WssConnection& conn);

    // 执行中间件链（不使用 std::function 传递 next：见 .cpp 的实现注释）
    void executeChain(const PathPlan& plan, WssConnection& conn, WsMessage& msg);
};

}  // namespace wss
}  // namespace http
