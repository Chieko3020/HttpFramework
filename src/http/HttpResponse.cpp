#include "http/HttpResponse.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <filesystem>

namespace http {

HttpResponse::HttpResponse() 
    : statusCode_(200), reasonPhrase_("OK"), sent_(false) {
    // 设置默认头部
    setHeader("Server", "HttpFramework/1.0");
    setHeader("Date", getCurrentTime());
}

void HttpResponse::setStatus(HttpStatus status) {
    statusCode_ = static_cast<int>(status);
    reasonPhrase_ = getReasonPhrase(statusCode_);
}

void HttpResponse::setStatus(int statusCode, const std::string& reasonPhrase) {
    statusCode_ = statusCode;
    reasonPhrase_ = reasonPhrase.empty() ? getReasonPhrase(statusCode) : reasonPhrase;
}

void HttpResponse::setHeader(const std::string& name, const std::string& value) {
    headers_[name] = value;
}

void HttpResponse::setContentType(const std::string& contentType) {
    setHeader("Content-Type", contentType);
}

void HttpResponse::setContentLength(size_t length) {
    setHeader("Content-Length", std::to_string(length));
}

void HttpResponse::setBody(const std::string& body) {
    body_ = body;
    setContentLength(body.length());
}

void HttpResponse::setBody(const char* data, size_t length) {
    body_ = std::string(data, length);
    setContentLength(length);
}

void HttpResponse::setJson(const std::string& json) {
    setContentType("application/json; charset=utf-8");
    setBody(json);
}

void HttpResponse::setHtml(const std::string& html) {
    setContentType("text/html; charset=utf-8");
    setBody(html);
}

void HttpResponse::setText(const std::string& text) {
    setContentType("text/plain; charset=utf-8");
    setBody(text);
}

void HttpResponse::setFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        setStatus(HttpStatus::NOT_FOUND);
        setText("File not found");
        return;
    }
    
    // 读取文件内容
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    
    // 根据文件扩展名设置Content-Type
    std::filesystem::path path(filePath);
    std::string extension = path.extension().string();
    
    if (extension == ".html" || extension == ".htm") {
        setContentType("text/html; charset=utf-8");
    } else if (extension == ".css") {
        setContentType("text/css");
    } else if (extension == ".js") {
        setContentType("application/javascript");
    } else if (extension == ".json") {
        setContentType("application/json");
    } else if (extension == ".png") {
        setContentType("image/png");
    } else if (extension == ".jpg" || extension == ".jpeg") {
        setContentType("image/jpeg");
    } else if (extension == ".gif") {
        setContentType("image/gif");
    } else if (extension == ".svg") {
        setContentType("image/svg+xml");
    } else {
        setContentType("application/octet-stream");
    }
    
    setBody(content);
}

void HttpResponse::redirect(const std::string& url, HttpStatus status) {
    setStatus(status);
    setHeader("Location", url);
    setBody("");
}

const std::string& HttpResponse::getHeader(const std::string& name) const {
    static const std::string empty;
    auto it = headers_.find(name);
    return (it != headers_.end()) ? it->second : empty;
}

std::string HttpResponse::toString(bool suppressBody) const {
    std::ostringstream oss;
    
    // 状态行
    oss << "HTTP/1.1 " << statusCode_ << " " << reasonPhrase_ << "\r\n";
    
    // 头部
    for (const auto& header : headers_) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    
    // 空行
    oss << "\r\n";
    
    // 响应体（HEAD 请求只发头部，但 Content-Length 仍是完整长度）
    if (!suppressBody) {
        oss << body_;
    }
    
    return oss.str();
}

void HttpResponse::clear() {
    statusCode_ = 200;
    reasonPhrase_ = "OK";
    headers_.clear();
    body_.clear();
    sent_ = false;
    
    // 重新设置默认头部
    setHeader("Server", "HttpFramework/1.0");
    setHeader("Date", getCurrentTime());
}

std::string HttpResponse::getReasonPhrase(int statusCode) const {
    // RFC 9110 §15 常用状态码（此前缺 3xx，redirect() 默认 302 会输出
    // "HTTP/1.1 302 Unknown"，不合规且易被客户端/代理拒绝）（M9）
    switch (statusCode) {
        // 2xx
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        // 3xx
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        // 4xx
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 418: return "I'm a teapot";
        case 422: return "Unprocessable Content";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        // 5xx
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        // 未知码：不再输出 "Unknown"（那是非标准的伪原因短语），
        // 按 RFC 9110 允许空原因短语，状态行仍带完整数字码
        default: return "";
    }
}

std::string HttpResponse::getCurrentTime() const {
    std::time_t now = std::time(nullptr);
    std::tm tm;
    ::gmtime_r(&now, &tm);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

} // namespace http
