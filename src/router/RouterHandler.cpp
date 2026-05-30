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
    // 设置默认404处理器
    setNotFoundHandler([]([[maybe_unused]] const http::HttpRequest& req, http::HttpResponse& res) {
        res.setStatus(http::HttpStatus::NOT_FOUND);
        res.setHtml("<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
    });
    
    // 设置默认错误处理器
    setErrorHandler([](const std::exception& e, [[maybe_unused]] const http::HttpRequest& req, http::HttpResponse& res) {
        res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
        res.setHtml("<html><body><h1>500 Internal Server Error</h1><p>An error occurred while processing your request.</p></body></html>");
        std::cerr << "Error handling request: " << e.what() << std::endl;
    });
}

void RouterHandler::addRoute(const std::string& method, const std::string& path, 
                            std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler) {
    routes_.emplace_back(method, path, handler);
}

void RouterHandler::addMiddleware(const std::string& path, 
                                 std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware) {
    middlewares_.emplace_back(path, middleware);
}

bool RouterHandler::handleRequest(const http::HttpRequest& request, http::HttpResponse& response) {
    try {
        // 查找匹配的中间件
        auto matchedMiddlewares = findMiddlewares(request.getPath());
        
        // 执行中间件链
        if (!matchedMiddlewares.empty()) {
            executeMiddlewareChain(request, response, [this, &request, &response]() {
                // 中间件执行完毕，处理路由
                std::unordered_map<std::string, std::string> params;
                Route* route = findRoute(request.getMethodString(), request.getPath(), params);
                
                if (route) {
                    // 设置路径参数
                    for (const auto& param : params) {
                        const_cast<http::HttpRequest&>(request).setParam(param.first, param.second);
                    }
                    route->handler(request, response);
                } else {
                    // 没有找到匹配的路由
                    if (notFoundHandler_) {
                        notFoundHandler_(request, response);
                    }
                }
            }, 0);
        } else {
            // 没有中间件，直接处理路由
            std::unordered_map<std::string, std::string> params;
            Route* route = findRoute(request.getMethodString(), request.getPath(), params);
            
            if (route) {
                // 设置路径参数
                for (const auto& param : params) {
                    const_cast<http::HttpRequest&>(request).setParam(param.first, param.second);
                }
                route->handler(request, response);
            } else {
                // 没有找到匹配的路由
                if (notFoundHandler_) {
                    notFoundHandler_(request, response);
                }
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        if (errorHandler_) {
            errorHandler_(e, request, response);
        }
        return false;
    }
}

void RouterHandler::setNotFoundHandler(std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler) {
    notFoundHandler_ = handler;
}

void RouterHandler::setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler) {
    errorHandler_ = handler;
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

void RouterHandler::executeMiddlewareChain(const http::HttpRequest& request, http::HttpResponse& response,
                                          std::function<void()> next, size_t index) {
    auto matchedMiddlewares = findMiddlewares(request.getPath());
    
    if (index >= matchedMiddlewares.size()) {
        next();
        return;
    }
    
    auto middleware = matchedMiddlewares[index];
    middleware->middleware(request, response, [this, &request, &response, next, index]() {
        executeMiddlewareChain(request, response, next, index + 1);
    });
}

Route* RouterHandler::findRoute(const std::string& method, const std::string& path, 
                               std::unordered_map<std::string, std::string>& params) {
    for (auto& route : routes_) {
        if (route.method == method && matchPath(route.pathRegex, route.paramNames, path, params)) {
            return &route;
        }
    }
    return nullptr;
}

std::vector<MiddlewareInfo*> RouterHandler::findMiddlewares(const std::string& path) {
    std::vector<MiddlewareInfo*> matched;
    
    for (auto& middleware : middlewares_) {
        if (std::regex_match(path, middleware.pathRegex)) {
            matched.push_back(&middleware);
        }
    }
    
    return matched;
}

} // namespace router
