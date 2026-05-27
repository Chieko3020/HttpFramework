#pragma once

#include <memory>
#include <string>
#include <functional>
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "session/SessionManager.h"

namespace middleware {

class SessionMiddleware {
public:
    explicit SessionMiddleware(std::shared_ptr<session::SessionManager> sessionManager);
    ~SessionMiddleware() = default;
    
    // 中间件函数
    void operator()(const http::HttpRequest& request, http::HttpResponse& response, std::function<void()> next);
    
    // 设置Cookie选项
    void setCookieName(const std::string& name) { cookieName_ = name; }
    void setCookiePath(const std::string& path) { cookiePath_ = path; }
    void setCookieDomain(const std::string& domain) { cookieDomain_ = domain; }
    void setSecure(bool secure) { secure_ = secure; }
    void setHttpOnly(bool httpOnly) { httpOnly_ = httpOnly; }
    
    // 获取会话管理器
    std::shared_ptr<session::SessionManager> getSessionManager() const { return sessionManager_; }

private:
    std::shared_ptr<session::SessionManager> sessionManager_;
    std::string cookieName_;
    std::string cookiePath_;
    std::string cookieDomain_;
    bool secure_;
    bool httpOnly_;
    
    // 从请求中提取会话ID
    std::string extractSessionId(const http::HttpRequest& request);
    
    // 设置会话Cookie
    void setSessionCookie(http::HttpResponse& response, const std::string& sessionId);
    
    // 解析Cookie字符串
    std::unordered_map<std::string, std::string> parseCookies(const std::string& cookieHeader);
};

// 会话辅助函数
namespace session_utils {
    // 从HttpContext中获取会话
    std::shared_ptr<session::Session> getSession(const http::HttpRequest& request);
    
    // 设置会话到HttpContext
    void setSession(http::HttpRequest& request, std::shared_ptr<session::Session> session);
    
    // 创建会话中间件的便捷函数
    std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> 
    createSessionMiddleware(std::shared_ptr<session::SessionManager> sessionManager);
}

} // namespace middleware
