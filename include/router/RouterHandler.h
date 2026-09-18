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
    // isStatic=true 时只做整串哈希比较；否则按 pathSegs 分段比较，
    // 只有段内含正则元字符（即 '*' 通配）时才走 pathRegex。
    std::vector<std::string> patternSegs;
    // 段下标 → paramNames 的下标一致（pathToRegex 按出现顺序收集），
    // 因此第 k 个动态段对应的参数名是 paramNames[k]。
    std::vector<std::size_t> paramSegments;
    bool isStatic{false};
    bool needsRegex{false};

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
        // 动态路径下标，注册顺序（决定匹配优先级，与原线性扫描一致）
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

    // 在一个方法的路由视图里找匹配项（精确 → 动态）
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
