#include "middleware/SessionMiddleware.h"
#include <sstream>
#include <algorithm>

namespace middleware {

SessionMiddleware::SessionMiddleware(std::shared_ptr<session::SessionManager> sessionManager)
    : sessionManager_(sessionManager), cookieName_("session_id"), cookiePath_("/"), 
      secure_(false), httpOnly_(true) {
}

void SessionMiddleware::operator()(const http::HttpRequest& request, http::HttpResponse& response, std::function<void()> next) {
    // 从请求中提取会话ID
    std::string sessionId = extractSessionId(request);
    
    std::shared_ptr<session::Session> session;
    
    if (!sessionId.empty()) {
        // 尝试获取现有会话
        session = sessionManager_->getSession(sessionId);
    }
    
    if (!session) {
        // 创建新会话
        session = sessionManager_->createSession();
        setSessionCookie(response, session->getId());
    }
    
    // 将会话添加到请求上下文中
    const_cast<http::HttpRequest&>(request).setUserData("session", session->getId());
    
    // 继续处理请求
    next();
    
    // 如果会话被修改，更新Cookie
    if (session && session->isValid()) {
        setSessionCookie(response, session->getId());
    }
}

std::string SessionMiddleware::extractSessionId(const http::HttpRequest& request) {
    std::string cookieHeader = request.getHeader("cookie");
    if (cookieHeader.empty()) {
        return "";
    }
    
    auto cookies = parseCookies(cookieHeader);
    auto it = cookies.find(cookieName_);
    return (it != cookies.end()) ? it->second : "";
}

void SessionMiddleware::setSessionCookie(http::HttpResponse& response, const std::string& sessionId) {
    std::ostringstream cookie;
    cookie << cookieName_ << "=" << sessionId;
    cookie << "; Path=" << cookiePath_;
    
    if (!cookieDomain_.empty()) {
        cookie << "; Domain=" << cookieDomain_;
    }
    
    if (secure_) {
        cookie << "; Secure";
    }
    
    if (httpOnly_) {
        cookie << "; HttpOnly";
    }
    
    // 设置过期时间（会话过期时间）
    cookie << "; Max-Age=" << sessionManager_->getSessionCount(); // 这里应该使用实际的过期时间
    
    response.setHeader("Set-Cookie", cookie.str());
}

std::unordered_map<std::string, std::string> SessionMiddleware::parseCookies(const std::string& cookieHeader) {
    std::unordered_map<std::string, std::string> cookies;
    
    std::istringstream stream(cookieHeader);
    std::string cookie;
    
    while (std::getline(stream, cookie, ';')) {
        // 去除前后空格
        cookie.erase(0, cookie.find_first_not_of(" \t"));
        cookie.erase(cookie.find_last_not_of(" \t") + 1);
        
        size_t equalPos = cookie.find('=');
        if (equalPos != std::string::npos) {
            std::string name = cookie.substr(0, equalPos);
            std::string value = cookie.substr(equalPos + 1);
            
            // 去除值的引号
            if (!value.empty() && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }
            
            cookies[name] = value;
        }
    }
    
    return cookies;
}

namespace session_utils {

std::shared_ptr<session::Session> getSession(const http::HttpRequest& request) {
    std::string sessionId = request.getUserData("session");
    if (sessionId.empty()) {
        return nullptr;
    }
    
    // 这里需要访问SessionManager，但HttpRequest中没有直接的访问方式
    // 在实际使用中，应该通过其他方式获取SessionManager实例
    return nullptr;
}

void setSession(http::HttpRequest& request, std::shared_ptr<session::Session> session) {
    if (session) {
        request.setUserData("session", session->getId());
    }
}

std::function<void(const http::HttpRequest&, http::HttpResponse&, std::function<void()>)> 
createSessionMiddleware(std::shared_ptr<session::SessionManager> sessionManager) {
    auto middleware = std::make_shared<SessionMiddleware>(sessionManager);
    return [middleware](const http::HttpRequest& request, http::HttpResponse& response, std::function<void()> next) {
        (*middleware)(request, response, next);
    };
}

} // namespace session_utils

} // namespace middleware
