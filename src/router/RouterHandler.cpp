#include "router/RouterHandler.h"
#include <iostream>
#include <regex>
#include <algorithm>

namespace router {

Route::Route(const std::string& method, const std::string& path, 
             std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler)
    : method(method), path(path), handler(handler) {
    
    // 将路径模式转换为正则表达式
    auto [regex, paramNames] = RouterHandler::pathToRegex(path);
    pathRegex = regex;
    this->paramNames = paramNames;
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
        auto matchedMiddlewares = findMiddlewares(*table, request.getPath());

        if (!matchedMiddlewares.empty()) {
            executeMiddlewareChain(*table, request, response,
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

void RouterHandler::executeMiddlewareChain(const Table& table, const http::HttpRequest& request,
                                          http::HttpResponse& response, std::function<void()> next,
                                          size_t index) {
    auto matchedMiddlewares = findMiddlewares(table, request.getPath());
    
    if (index >= matchedMiddlewares.size()) {
        next();
        return;
    }
    
    const MiddlewareInfo* middleware = matchedMiddlewares[index];
    // table 由调用方的 shared_ptr 保活，这里取到的指针在整个调用链期间有效
    middleware->middleware(request, response, [&table, &request, &response, next, index]() {
        executeMiddlewareChain(table, request, response, next, index + 1);
    });
}

const Route* RouterHandler::findRoute(const Table& table, const std::string& method,
                                      const std::string& path,
                                      std::unordered_map<std::string, std::string>& params) {
    for (const auto& route : table.routes) {
        if (route.method == method && matchPath(route.pathRegex, route.paramNames, path, params)) {
            return &route;
        }
    }
    // HEAD 请求回退到 GET handler（RFC 9110 §9.3.2）：
    // 响应体由 HttpServer 在序列化时抑制，因此可以安全复用 GET 的实现（H13）
    if (method == "HEAD") {
        for (const auto& route : table.routes) {
            if (route.method == "GET" &&
                matchPath(route.pathRegex, route.paramNames, path, params)) {
                return &route;
            }
        }
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
