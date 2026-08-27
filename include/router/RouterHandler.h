#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <regex>
#include <memory>
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

namespace router {

struct Route {
    std::string method;
    std::string path;
    std::regex pathRegex;
    std::vector<std::string> paramNames;
    std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler;
    
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
    void addRoute(const std::string& method, const std::string& path, 
                  std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 添加中间件
    void addMiddleware(const std::string& path, 
                       std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> middleware);
    
    // 处理请求
    bool handleRequest(const http::HttpRequest& request, http::HttpResponse& response);
    
    // 设置404处理器
    void setNotFoundHandler(std::function<void(const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 设置错误处理器
    void setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler);
    
    // 将路径模式转换为正则表达式
    static std::pair<std::regex, std::vector<std::string>> pathToRegex(const std::string& path);

private:
    std::vector<Route> routes_;
    std::vector<MiddlewareInfo> middlewares_;
    std::function<void(const http::HttpRequest&, http::HttpResponse&)> notFoundHandler_;
    std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> errorHandler_;
    
    // 匹配路径并提取参数
    bool matchPath(const std::regex& pathRegex, const std::vector<std::string>& paramNames,
                   const std::string& requestPath, std::unordered_map<std::string, std::string>& params);
    
    // 执行中间件链
    void executeMiddlewareChain(const http::HttpRequest& request, http::HttpResponse& response,
                                std::function<void()> next, size_t index);
    
    // 查找匹配的路由
    Route* findRoute(const std::string& method, const std::string& path, 
                     std::unordered_map<std::string, std::string>& params);
    
    // 查找匹配的中间件
    std::vector<MiddlewareInfo*> findMiddlewares(const std::string& path);
};

} // namespace router
