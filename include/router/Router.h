#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <regex>
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "RouterHandler.h"

namespace router {

class Router {
public:
    using Handler = std::function<void(const http::HttpRequest&, http::HttpResponse&)>;
    using Middleware = std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)>;
    
    Router();
    ~Router() = default;
    
    // 添加路由
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);
    void del(const std::string& path, Handler handler);
    void patch(const std::string& path, Handler handler);
    void head(const std::string& path, Handler handler);
    void options(const std::string& path, Handler handler);
    
    // 添加任意方法的路由
    void addRoute(const std::string& method, const std::string& path, Handler handler);
    
    // 添加中间件
    void use(Middleware middleware);
    void use(const std::string& path, Middleware middleware);
    
    // 处理请求
    bool handleRequest(const http::HttpRequest& request, http::HttpResponse& response);
    
    // 设置404处理器
    void setNotFoundHandler(Handler handler);
    
    // 设置错误处理器
    void setErrorHandler(std::function<void(const std::exception&, const http::HttpRequest&, http::HttpResponse&)> handler);

private:
    std::unique_ptr<RouterHandler> handler_;
    
    // 添加路由的通用方法
    void addRouteInternal(const std::string& method, const std::string& path, Handler handler);
};

} // namespace router
