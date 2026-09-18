// test_wss_router.cpp — WsRouter 路径计划缓存的回归测试
//
// 背景（第三轮修复引入的两个缺陷，本用例是它们的判别性回归）：
//   1) 缓存 PathPlan 曾保存 routes_/globalMws_/scopedMws_ 的**裸指针**，而这些
//      容器是 std::vector：缓存建立后再注册任何路由/中间件，push_back 触发扩容
//      即让缓存里的指针悬空 → 下一条消息 heap-use-after-free（ASan 下两条栈：
//      extractParams 读已释放的 pathRegex、ChainRunner::run 调已释放的
//      std::function）。修复：容器改 std::deque（插入不搬移已存在元素）+
//      planFor 返回 shared_ptr 保活。
//   2) 缓存会把"该路径没有匹配 handler"这个**否定结论**也存下来，而注册时
//      不清缓存 → dispatch 之后再注册的路由/中间件永久收不到消息（而 onOpen/
//      onClose 走实时表、照常生效，行为不一致）。修复：注册时清空缓存。
//
// 判别力：把 include/HttpFramework/wss/WsRouter.h 与 src/wss/WsRouter.cpp 回退到
// 修复前，本用例在 ASan 构建下必然报 heap-use-after-free（用例 1/2），
// 或在普通构建下失败于"先 dispatch 后注册"（用例 3）。
//
// 说明：本文件只驱动 WsRouter 本身（不建 socket/TLS），因此不依赖
// cert/key 生成，也不需要 ENABLE_WSS 以外的环境。

#include "HttpFramework/wss/WsRouter.h"
#include "HttpFramework/wss/WssConnection.h"

#include <iostream>
#include <string>
#include <vector>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（只计入 g_testsSkipped，并置 g_skipFlag 让 run() 不要把它
//       算成通过；返回值是 false，因此退出码语义不变 —— 跳过不算失败）
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; g_skipFlag = true; return false; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;
[[maybe_unused]] static bool g_skipFlag = false;

namespace {

int g_routeHits = 0;
int g_mwHits = 0;

http::wss::WsMessage textMsg(const std::string& s) {
    http::wss::WsMessage m;
    m.opcode = 0x1;
    m.payload.assign(s.begin(), s.end());
    return m;
}

// 注册足够多的路由，保证任何先前建立的缓存指针都会因容器扩容而失效
void registerFillers(const std::shared_ptr<http::wss::WsRouter>& r, int n) {
    for (int i = 0; i < n; ++i) {
        r->addHandler("/filler" + std::to_string(i),
                      [](http::WssConnection&, const http::wss::WsMessage&) {});
    }
}

}  // namespace

// ── 用例 1：缓存后注册路由 → 参数提取路径不得 UAF ──
static bool test_plan_cache_survives_late_route_registration() {
    TEST("计划缓存后注册路由：handler 仍被调用且不悬空（ASan 下判别）");
    auto router = std::make_shared<http::wss::WsRouter>();
    router->addHandler("/a/:id", [](http::WssConnection&, const http::wss::WsMessage&) {
        ++g_routeHits;
    });

    http::WssConnection conn(-1, nullptr, 1);
    http::wss::WsMessage msg = textMsg("hi");

    g_routeHits = 0;
    router->dispatch("/a/1", conn, msg);          // ① 建立缓存
    CHECK(g_routeHits == 1, "首次 dispatch 应命中, 实际 " << g_routeHits);

    registerFillers(router, 64);                  // ② 注册 → 容器增长

    router->dispatch("/a/2", conn, msg);          // ③ 复用缓存：修复前此处 UAF
    CHECK(g_routeHits == 2, "注册后 dispatch 应仍命中, 实际 " << g_routeHits);

    // 参数提取走的是缓存里的 RouteEntry：退化成字符串签名能同时抓住
    // "指针悬空"与"参数名/正则被搬移后错乱"两类问题
    const std::string id = conn.getUserData("ws_param_id");
    CHECK(id == "2", "参数应仍被正确提取, 实际 '" << id << "'");
    PASS();
    return true;
}

// ── 用例 2：缓存后注册中间件 → 中间件链不得 UAF ──
static bool test_plan_cache_survives_late_middleware_registration() {
    TEST("计划缓存后注册中间件：链仍按新表执行且不悬空（ASan 下判别）");
    auto router = std::make_shared<http::wss::WsRouter>();
    router->addMiddleware([](http::WssConnection&, http::wss::WsMessage&, std::function<void()> next) {
        ++g_mwHits;
        next();
    });
    router->addHandler("/b", [](http::WssConnection&, const http::wss::WsMessage&) {});

    http::WssConnection conn(-1, nullptr, 1);
    http::wss::WsMessage msg = textMsg("x");

    g_mwHits = 0;
    router->dispatch("/b", conn, msg);            // ① 建立缓存（链里有 1 个指针）
    CHECK(g_mwHits == 1, "首次 dispatch 中间件应执行一次, 实际 " << g_mwHits);

    for (int i = 0; i < 64; ++i) {               // ② 注册 → globalMws_ 增长
        router->addMiddleware([](http::WssConnection&, http::wss::WsMessage&, std::function<void()> next) {
            next();
        });
    }

    router->dispatch("/b", conn, msg);            // ③ 复用旧计划：修复前此处 UAF
    CHECK(g_mwHits == 2, "注册后 dispatch 中间件应仍执行, 实际 " << g_mwHits);
    PASS();
    return true;
}

// ── 用例 3：陈旧否定缓存不得屏蔽后注册的路由/中间件 ──
static bool test_registration_invalidates_negative_cache() {
    TEST("先 dispatch 后注册：新路由与新中间件必须立即生效");
    auto router = std::make_shared<http::wss::WsRouter>();
    http::WssConnection conn(-1, nullptr, 1);
    http::wss::WsMessage msg = textMsg("x");

    // (a) 先给一条不存在的路径建立"无路由"缓存
    router->dispatch("/late", conn, msg);

    // (b) 再注册：必须立即生效（修复前被否定缓存永久屏蔽）
    int hits = 0;
    router->addHandler("/late", [&hits](http::WssConnection&, const http::wss::WsMessage&) {
        ++hits;
    });
    router->dispatch("/late", conn, msg);
    router->dispatch("/late", conn, msg);
    CHECK(hits == 2, "dispatch 之后注册的路由应收到消息, 实际 " << hits);

    // (c) 中间件同理：先走一次再注册全局中间件
    auto r2 = std::make_shared<http::wss::WsRouter>();
    r2->addHandler("/m", [](http::WssConnection&, const http::wss::WsMessage&) {});
    r2->dispatch("/m", conn, msg);
    int mw = 0;
    r2->addMiddleware([&mw](http::WssConnection&, http::wss::WsMessage&, std::function<void()> next) {
        ++mw;
        next();
    });
    r2->dispatch("/m", conn, msg);
    CHECK(mw == 1, "dispatch 之后注册的中间件应执行, 实际 " << mw);

    // (d) 生命周期回调与 dispatch 观感一致（都在注册后立即生效）
    int opened = 0;
    router->setOpenHandler("/late", [&opened](http::WssConnection&) { ++opened; });
    router->onOpen("/late", conn);
    CHECK(opened == 1, "onOpen 应看到注册后的路由, 实际 " << opened);
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_wss_router ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;
        if (fn()) {
            if (!g_skipFlag) ++g_testsPassed;
        } else if (!g_skipFlag) {
            ++g_testsFailed;
        }
    };

    run(test_plan_cache_survives_late_route_registration,
        "计划缓存后注册路由");
    run(test_plan_cache_survives_late_middleware_registration,
        "计划缓存后注册中间件");
    run(test_registration_invalidates_negative_cache,
        "注册使否定缓存失效");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过"
              << "（总用例 " << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）"
              << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
