#include "router/RouterHandler.h"
#include <iostream>
#include <regex>
#include <algorithm>

namespace router {

namespace {

// 把模式路径按 '/' 切段（保留空段语义：不做归一化，避免改变既有匹配行为）
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segs;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = path.find('/', start);
        if (pos == std::string::npos) {
            segs.push_back(path.substr(start));
            break;
        }
        segs.push_back(path.substr(start, pos - start));
        start = pos + 1;
    }
    return segs;
}

// 分段精确匹配：静态段必须全等，动态段（:param）通过 dynamic 传出
bool matchSegments(const std::vector<std::string>& patternSegs,
                   const std::vector<std::string>& pathSegs,
                   std::vector<std::size_t>* dynamic) {
    if (patternSegs.size() != pathSegs.size()) return false;
    if (dynamic) dynamic->clear();
    for (std::size_t i = 0; i < patternSegs.size(); ++i) {
        const std::string& p = patternSegs[i];
        if (p.size() > 1 && p.front() == ':') {
            if (pathSegs[i].empty()) return false;   // ([^/]+) 不允许空段
            if (dynamic) dynamic->push_back(i);
        } else if (p != pathSegs[i]) {
            return false;
        }
    }
    return true;
}

// 模式段是否含正则元字符（此时分段匹配不适用，必须回退 std::regex）
bool hasRegexMeta(const std::string& seg) {
    static const std::string meta = R"(.*+?^${}()|[]\)";
    return seg.find_first_of(meta) != std::string::npos;
}

}  // namespace

Route::Route(const std::string& method, const std::string& path, 
             std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler)
    : method(method), path(path), handler(handler) {
    
    // 将路径模式转换为正则表达式
    auto [regex, paramNames] = RouterHandler::pathToRegex(path);
    pathRegex = regex;
    this->paramNames = paramNames;

    // ── 一次性的匹配计划（构造期算好，请求期只做字符串比较）──
    // 原实现每个请求对每条路由做 std::regex_match（O(路由数) 次正则），
    // 这里把"这条路由怎么匹配"提前算成：只含静态段的段向量 / 动态段下标。
    patternSegs = splitPath(path);
    bool staticOnly = true;
    for (std::size_t i = 0; i < patternSegs.size(); ++i) {
        const std::string& seg = patternSegs[i];
        if (seg.size() > 1 && seg.front() == ':') {
            staticOnly = false;
            paramSegments.push_back(i);
        } else if (hasRegexMeta(seg)) {
            // '*' 通配（以及任何正则元字符）无法用分段精确匹配表达
            staticOnly = false;
            needsRegex = true;
        }
    }
    isStatic = staticOnly;
}

void RouterHandler::Table::rebuildIndex() {
    methodIndex.clear();
    for (std::size_t i = 0; i < routes.size(); ++i) {
        MatchIndex& idx = methodIndex[routes[i].method];
        if (routes[i].isStatic) {
            // 先注册者优先：原线性扫描遇到同路径重复注册时用第一条，这里保持
            idx.exact.emplace(routes[i].path, i);
        } else {
            idx.dynamicRoutes.push_back(i);
        }
    }
}

MiddlewareInfo::MiddlewareInfo(const std::string& path, 
                               std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware)
    : path(path), middleware(middleware) {
    
    if (path.empty()) {
        // 全局中间件，匹配所有路径
        pathRegex = std::regex(".*");
    } else {
        auto [regex, paramNames] = RouterHandler::pathToRegex(path);
        pathRegex = regex;
    }
}

RouterHandler::RouterHandler() {
    auto table = std::make_shared<Table>();

    // 设置默认404处理器
    table->notFoundHandler = []([[maybe_unused]] const http::HttpRequest& req, http::HttpResponse& res) {
        res.setStatus(http::HttpStatus::NOT_FOUND);
        res.setHtml("<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
    };
    
    // 设置默认错误处理器
    table->errorHandler = [](const std::exception& e, [[maybe_unused]] const http::HttpRequest& req, http::HttpResponse& res) {
        res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
        res.setHtml("<html><body><h1>500 Internal Server Error</h1><p>An error occurred while processing your request.</p></body></html>");
        std::cerr << "[ERROR][路由]：请求处理错误: " << e.what() << std::endl;
    };

    std::atomic_store(&table_, std::shared_ptr<const Table>(std::move(table)));
}

std::shared_ptr<const RouterHandler::Table> RouterHandler::snapshot() const {
    return std::atomic_load(&table_);
}

void RouterHandler::mutate(const std::function<void(Table&)>& fn) {
    // 拷贝-修改-换入：注册期间请求侧始终读到一份完整、不变的旧表（H14）
    std::shared_ptr<const Table> current = std::atomic_load(&table_);
    auto next = std::make_shared<Table>(*current);
    fn(*next);
    next->rebuildIndex();   // 索引跟着 routes 一起维护，只在这里重建
    std::atomic_store(&table_, std::shared_ptr<const Table>(std::move(next)));
}

void RouterHandler::addRoute(const std::string& method, const std::string& path, 
                            std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler) {
    mutate([&](Table& t) { t.routes.emplace_back(method, path, handler); });
}

void RouterHandler::addMiddleware(const std::string& path, 
                                 std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware) {
    mutate([&](Table& t) { t.middlewares.emplace_back(path, middleware); });
}

void RouterHandler::setNotFoundHandler(std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler) {
    mutate([&](Table& t) { t.notFoundHandler = std::move(handler); });
}

void RouterHandler::setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler) {
    mutate([&](Table& t) { t.errorHandler = std::move(handler); });
}

bool RouterHandler::handleRequest(const http::HttpRequest& request, http::HttpResponse& response) {
    // 一次原子读拿到本次请求的不可变表快照，全程（含中间件里的延迟调用）使用它
    std::shared_ptr<const Table> table = snapshot();

    try {
        // 中间件只在这一处扫描一次，整条链复用这份列表（原实现每层递归重扫）
        auto matchedMiddlewares = findMiddlewares(*table, request.getPath());

        if (!matchedMiddlewares.empty()) {
            executeMiddlewareChain(request, response, matchedMiddlewares,
                                   [&table, &request, &response]() {
                                       dispatchRoute(*table, request, response);
                                   },
                                   0);
        } else {
            dispatchRoute(*table, request, response);
        }
        
        return true;
    } catch (const std::exception& e) {
        if (table->errorHandler) {
            table->errorHandler(e, request, response);
        }
        return false;
    }
}

void RouterHandler::dispatchRoute(const Table& table, const http::HttpRequest& request,
                                  http::HttpResponse& response) {
    std::unordered_map<std::string, std::string> params;
    const Route* route = findRoute(table, request.getMethodString(), request.getPath(), params);

    if (route) {
        for (const auto& param : params) {
            const_cast<http::HttpRequest&>(request).setParam(param.first, param.second);
        }
        route->handler(request, response);
    } else if (table.notFoundHandler) {
        table.notFoundHandler(request, response);
    }
}

std::pair<std::regex, std::vector<std::string>> RouterHandler::pathToRegex(const std::string& path) {
    std::vector<std::string> paramNames;
    std::string regexStr = path;
    
    // 转义特殊字符
    std::string escaped = std::regex_replace(regexStr, std::regex(R"([.*+?^${}()|[\]\\])"), R"(\$&)");
    
    // 替换参数占位符 :param
    std::regex paramRegex(R"(:([a-zA-Z_][a-zA-Z0-9_]*))");
    std::sregex_iterator begin(escaped.begin(), escaped.end(), paramRegex);
    std::sregex_iterator end;
    
    std::string result = escaped;
    size_t offset = 0;
    
    for (auto it = begin; it != end; ++it) {
        std::string paramName = (*it)[1].str();
        paramNames.push_back(paramName);
        
        std::string replacement = "([^/]+)";
        size_t pos = result.find((*it)[0].str(), offset);
        if (pos != std::string::npos) {
            result.replace(pos, (*it)[0].length(), replacement);
            offset = pos + replacement.length();
        }
    }
    
    // 替换通配符 *（需要匹配转义后的 \*）
    result = std::regex_replace(result, std::regex(R"(\\\*)"), R"((.*))");
    
    return {std::regex("^" + result + "$"), paramNames};
}

bool RouterHandler::matchPath(const std::regex& pathRegex, const std::vector<std::string>& paramNames,
                             const std::string& requestPath, std::unordered_map<std::string, std::string>& params) {
    std::smatch matches;
    if (std::regex_match(requestPath, matches, pathRegex)) {
        for (size_t i = 0; i < paramNames.size() && i + 1 < matches.size(); ++i) {
            params[paramNames[i]] = matches[i + 1].str();
        }
        return true;
    }
    return false;
}

void RouterHandler::executeMiddlewareChain(const http::HttpRequest& request,
                                          http::HttpResponse& response,
                                          const std::vector<const MiddlewareInfo*>& matched,
                                          const std::function<void()>& next,
                                          size_t index) {
    if (index >= matched.size()) {
        next();
        return;
    }

    const MiddlewareInfo* middleware = matched[index];
    // matched 由 handleRequest 的栈帧持有，覆盖整条链（含中间件里的同步/异步 next()）
    middleware->middleware(request, response, [&request, &response, &matched, &next, index]() {
        executeMiddlewareChain(request, response, matched, next, index + 1);
    });
}

const Route* RouterHandler::matchMethod(const Table& table, const MatchIndex& index,
                                        const std::string& path,
                                        std::unordered_map<std::string, std::string>& params) {
    // ① 静态路由：一次哈希查找
    auto exactIt = index.exact.find(path);
    if (exactIt != index.exact.end()) return &table.routes[exactIt->second];

    // 快速退出：该方法下没有动态路由（纯静态路由表）时不做任何分段工作
    if (index.dynamicRoutes.empty()) return nullptr;

    // ② 动态路由：按注册顺序尝试，静态段全等 + :param 段非空
    std::vector<std::size_t> dynamicSegs;
    const std::vector<std::string> pathSegs = splitPath(path);
    for (std::size_t idx : index.dynamicRoutes) {
        const Route& route = table.routes[idx];
        if (!route.needsRegex) {
            if (!matchSegments(route.patternSegs, pathSegs, &dynamicSegs)) continue;
            for (std::size_t k = 0; k < dynamicSegs.size() && k < route.paramNames.size(); ++k)
                params[route.paramNames[k]] = pathSegs[dynamicSegs[k]];
            return &route;
        }
        // 含 '*' 等正则元字符：退化为正则匹配（构造期已编译好 pathRegex）
        if (matchPath(route.pathRegex, route.paramNames, path, params)) return &route;
    }
    return nullptr;
}

const Route* RouterHandler::findRoute(const Table& table, const std::string& method,
                                      const std::string& path,
                                      std::unordered_map<std::string, std::string>& params) {
    // 先按方法过滤，再做匹配（原实现对每条路由同时比较 method 与正则）
    auto it = table.methodIndex.find(method);
    if (it != table.methodIndex.end()) {
        const Route* hit = matchMethod(table, it->second, path, params);
        if (hit) return hit;
    }
    // HEAD 请求回退到 GET handler（RFC 9110 §9.3.2）：
    // 响应体由 HttpServer 在序列化时抑制，因此可以安全复用 GET 的实现（H13）
    if (method == "HEAD") {
        auto git = table.methodIndex.find("GET");
        if (git != table.methodIndex.end()) return matchMethod(table, git->second, path, params);
    }
    return nullptr;
}

std::vector<const MiddlewareInfo*> RouterHandler::findMiddlewares(const Table& table,
                                                                  const std::string& path) {
    std::vector<const MiddlewareInfo*> matched;
    
    for (const auto& middleware : table.middlewares) {
        if (std::regex_match(path, middleware.pathRegex)) {
            matched.push_back(&middleware);
        }
    }
    
    return matched;
}

} // namespace router
