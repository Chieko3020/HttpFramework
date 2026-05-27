#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace http {

enum class HttpStatus {
    OK = 200,
    CREATED = 201,
    NO_CONTENT = 204,
    FOUND = 302,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    METHOD_NOT_ALLOWED = 405,
    INTERNAL_SERVER_ERROR = 500,
    NOT_IMPLEMENTED = 501,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503
};

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse() = default;
    
    // 设置状态码
    void setStatus(HttpStatus status);
    void setStatus(int statusCode, const std::string& reasonPhrase = "");
    
    // 设置头部
    void setHeader(const std::string& name, const std::string& value);
    void setContentType(const std::string& contentType);
    void setContentLength(size_t length);
    
    // 设置响应体
    void setBody(const std::string& body);
    void setBody(const char* data, size_t length);
    
    // 设置JSON响应
    void setJson(const std::string& json);
    
    // 设置HTML响应
    void setHtml(const std::string& html);
    
    // 设置文本响应
    void setText(const std::string& text);
    
    // 设置文件响应
    void setFile(const std::string& filePath);
    
    // 重定向
    void redirect(const std::string& url, HttpStatus status = HttpStatus::FOUND);
    
    // 获取状态码
    int getStatusCode() const { return statusCode_; }
    
    // 获取原因短语
    const std::string& getReasonPhrase() const { return reasonPhrase_; }
    
    // 获取头部
    const std::string& getHeader(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& getHeaders() const { return headers_; }
    
    // 获取响应体
    const std::string& getBody() const { return body_; }
    
    // 生成HTTP响应字符串
    std::string toString() const;
    
    // 清空响应
    void clear();
    
    // 检查是否已发送
    bool isSent() const { return sent_; }
    void setSent(bool sent) { sent_ = sent; }

private:
    int statusCode_;
    std::string reasonPhrase_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    bool sent_;
    
    // 获取状态码对应的原因短语
    std::string getReasonPhrase(int statusCode) const;
    
    // 获取当前时间字符串（用于Date头部）
    std::string getCurrentTime() const;
};

} // namespace http
