#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <cstddef>
#include <cstdint>
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

namespace router {

struct Route {
    std::string method;
    std::string path;
    std::regex pathRegex;
    std::vector<std::string> paramNames;
    std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler;

    // ── 一次性算好的匹配计划（见 Route 构造函数）──
    // 分段匹配必须与 pathToRegex 的语法**严格等价**，否则同一份路径模式的
    // 匹配结果会因"走哪条快路径"而不同。因此只在一段一段都能一一对应时才启用
    // 分段快路径，判据（见 .cpp 的 isPureParamSeg / segmentHasColon）：
    //   * 每一段：不含 ':' 且不含正则元字符 → 静态段，按字符串全等比较；
    //   * 每一段：整段恰好是 ":标识符" → 动态段，等价于正则 ([^/]+)，且
    //     占位符个数 == paramNames 个数。
    // 任何其他形态（":name.json"、段中部 ":c"、同段两个 ":a:b"）都会**整条路由**
    // 退化为 pathRegex：这些形态在 pathToRegex 里要么吞掉静态后缀、要么把字面
    // ':' 当锚点、要么产生比 paramNames 更多的捕获组，分段匹配无法复刻。
    std::vector<std::string> patternSegs;   // 仅 needsRegex==false 时有意义
    bool isStatic{false};    // 纯静态：整串全等即可判定（无 ':' 且无正则元字符）
    bool needsRegex{false};  // 必须走 pathRegex（含 ':' 的复杂形态或 '*' 通配）

    Route(const std::string& method, const std::string& path, 
          std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler);
};

struct MiddlewareInfo {
    std::string path;
    std::regex pathRegex;
    std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware;
    
    MiddlewareInfo(const std::string& path, 
                   std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware);
};

class RouterHandler {
public:
    RouterHandler();
    ~RouterHandler() = default;
    
    // 添加路由
    //
    // 线程安全（H14）：注册采用"拷贝-修改-原子换入"（copy-on-write）。
    // 请求侧只做一次原子读拿到不可变快照，因此运行期动态注册路由/中间件
    // 不会与正在执行的请求竞争（旧实现整目录零锁，vector 扩容会搬移正在
    // 执行的 std::function）。代价是每次注册复制一份表——注册是低频操作。
    void addRoute(const std::string& method, const std::string& path, 
                  std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 添加中间件（线程安全，同上）
    void addMiddleware(const std::string& path, 
                       std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware);
    
    // 处理请求（内部取一次表快照，全程使用同一份不可变表）
    bool handleRequest(const http::HttpRequest& request, http::HttpResponse& response);
    
    // 设置404处理器
    void setNotFoundHandler(std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 设置错误处理器
    void setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 将路径模式转换为正则表达式
    static std::pair<std::regex, std::vector<std::string>> pathToRegex(const std::string& path);

private:
    // 单个方法的路由视图：静态路径走哈希表，动态路径（含 :param 或 *）才回退正则。
    // 指针指向 Table::routes 的元素，指向关系与 Table 的生命周期绑定（表是
    // 不可变快照：mutate() 拷贝-修改-换入，从不原地修改已发布的表）。
    struct MatchIndex {
        // 静态路径（无 :param / *，模式与请求路径全等）→ routes 下标
        std::unordered_map<std::string, std::size_t> exact;
        // 动态路径下标，注册顺序
        std::vector<std::size_t> dynamicRoutes;
    };

    // 路由表 + 匹配索引 + 中间件链 + 兜底处理器的一整份快照
    struct Table {
        std::vector<Route> routes;
        std::vector<MiddlewareInfo> middlewares;
        // method → 路由视图。由 rebuildIndex() 在每次变更后重建。
        std::unordered_map<std::string, MatchIndex> methodIndex;
        std::function<void(const http::HttpRequest&, http::HttpResponse&)> notFoundHandler;
        std::function<void(const std::exception&, const http::HttpRequest&,
                           http::HttpResponse&)> errorHandler;

        // 依据当前 routes 重建 methodIndex（只做字符串比较，不构造正则）
        void rebuildIndex();
    };

    // 当前生效的表（只通过 std::atomic_load/atomic_store 读写）
    std::shared_ptr<const Table> table_;

    std::shared_ptr<const Table> snapshot() const;
    // 拷贝-修改-换入
    void mutate(const std::function<void(Table&)>& fn);

    // 匹配路径并提取参数
    static bool matchPath(const std::regex& pathRegex, const std::vector<std::string>& paramNames,
                          const std::string& requestPath,
                          std::unordered_map<std::string, std::string>& params);

    // 在一个方法的路由视图里找匹配项
    // 优先级与原线性扫描一致：**按注册顺序**取第一条命中的路由；静态表的
    // 哈希命中不享有额外优先权（若它注册得比某条动态路由晚，动态路由先赢）。
    static const Route* matchMethod(const Table& table, const MatchIndex& index,
                                    const std::string& path,
                                    std::unordered_map<std::string, std::string>& params);
    
    // 执行中间件链
    // matched 由调用方（handleRequest）算好传进来：链的每一层不再重复做
    // O(M) 的正则扫描（原实现每层递归重新 findMiddlewares）。
    static void executeMiddlewareChain(const http::HttpRequest& request,
                                       http::HttpResponse& response,
                                       const std::vector<const MiddlewareInfo*>& matched,
                                       const std::function<void()>& next, size_t index);
    
    // 查找匹配的路由（HEAD 回退到 GET）
    static const Route* findRoute(const Table& table, const std::string& method,
                                  const std::string& path,
                                  std::unordered_map<std::string, std::string>& params);
    
    // 查找匹配的中间件
    static std::vector<const MiddlewareInfo*> findMiddlewares(const Table& table,
                                                              const std::string& path);

    // 执行最终路由（或 404）
    static void dispatchRoute(const Table& table, const http::HttpRequest& request,
                              http::HttpResponse& response);
};

} // namespace router
