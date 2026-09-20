#include "http/HttpResponse.h"
#include <sstream>
#include <mutex>
#include <cctype>
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

// 头名归一化（M10）：统一按小写键存储、按规范拼写输出。
// 旧实现直接以调用方给的拼写做键，"content-length" 与 "Content-Length" 会成为
// 两个不同的键 → 响应里出现重复的 Content-Length（响应拆分/缓存投毒面）。
std::string HttpResponse::canonicalHeaderName(const std::string& name) {
    static const std::pair<const char*, const char*> kKnown[] = {
        {"content-length", "Content-Length"},   {"content-type", "Content-Type"},
        {"set-cookie", "Set-Cookie"},           {"connection", "Connection"},
        {"server", "Server"},                   {"date", "Date"},
        {"location", "Location"},               {"transfer-encoding", "Transfer-Encoding"},
        {"cache-control", "Cache-Control"},     {"etag", "ETag"},
        {"last-modified", "Last-Modified"},     {"content-encoding", "Content-Encoding"},
        {"www-authenticate", "WWW-Authenticate"},
        {"access-control-allow-origin", "Access-Control-Allow-Origin"},
        {"access-control-allow-methods", "Access-Control-Allow-Methods"},
        {"access-control-allow-headers", "Access-Control-Allow-Headers"},
    };

    // 先求小写
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    // 去掉首尾空白
    const auto b = lower.find_first_not_of(" \t");
    if (b == std::string::npos) return lower;
    const auto e = lower.find_last_not_of(" \t");
    lower = lower.substr(b, e - b + 1);

    for (const auto& kv : kKnown) {
        if (lower == kv.first) return kv.second;
    }

    // 未知头：每个 '-' 分隔段的词首字母大写
    std::string out = lower;
    bool upper = true;
    for (char& c : out) {
        if (c == '-') { upper = true; continue; }
        if (upper) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); upper = false; }
    }
    return out;
}

void HttpResponse::setHeader(const std::string& name, const std::string& value) {
    headers_[canonicalHeaderName(name)] = value;
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
    // 归一化后查找：getHeader("content-type") 与 getHeader("Content-Type") 等价（M10）
    auto it = headers_.find(canonicalHeaderName(name));
    return (it != headers_.end()) ? it->second : empty;
}

std::string HttpResponse::toString(bool suppressBody) const {
    // 直接拼接 + 预留容量（M8）：旧实现用 ostringstream，除最终 str() 外
    // 还要维护其内部缓冲，每次响应至少多一次整块分配/拷贝。
    std::size_t reserveSize = 32 + reasonPhrase_.size() + body_.size();
    for (const auto& header : headers_) {
        reserveSize += header.first.size() + header.second.size() + 4;
    }

    std::string out;
    out.reserve(reserveSize);

    // 状态行
    out += "HTTP/1.1 ";
    out += std::to_string(statusCode_);
    out += ' ';
    out += reasonPhrase_;
    out += "\r\n";

    // 头部
    for (const auto& header : headers_) {
        out += header.first;
        out += ": ";
        out += header.second;
        out += "\r\n";
    }

    // 空行
    out += "\r\n";

    // 响应体（HEAD 请求只发头部，但 Content-Length 仍是完整长度）
    if (!suppressBody) {
        out += body_;
    }

    return out;
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
    // Date 按秒缓存（M8）：同一秒内的所有响应复用同一个字符串，
    // 不再每个响应都走一遍 gmtime + put_time + ostringstream。
    static std::mutex mu;
    static std::time_t cachedSecond = 0;
    static std::string cachedValue;

    const std::time_t now = std::time(nullptr);

    std::lock_guard<std::mutex> lk(mu);
    if (now == cachedSecond && !cachedValue.empty()) {
        return cachedValue;
    }

    std::tm tm;
    ::gmtime_r(&now, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    cachedSecond = now;
    cachedValue = oss.str();
    return cachedValue;
}

} // namespace http
