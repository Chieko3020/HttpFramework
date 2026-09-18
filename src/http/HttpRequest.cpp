#include "http/HttpRequest.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>

namespace http {

HttpRequest::HttpRequest() 
    : method_(HttpMethod::UNKNOWN), methodString_(""), path_(""), version_("") {
}

bool HttpRequest::parse(const std::string& rawRequest) {
    rawRequest_ = rawRequest;
    contentLengthValid_ = true;
    contentLength_ = 0;
    bodyConsumed_ = 0;
    rawBodySize_ = 0;
    conflictingFraming_ = false;
    bodyTooLarge_ = false;
    
    std::istringstream stream(rawRequest);
    std::string line;
    
    // 解析请求行
    if (!std::getline(stream, line)) {
        return false;
    }
    
    // 移除回车符
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    
    if (!parseRequestLine(line)) {
        return false;
    }
    
    // 解析头部
    std::vector<std::string> headerLines;
    while (std::getline(stream, line) && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        headerLines.push_back(line);
    }
    
    if (!parseHeaders(headerLines)) {
        return false;
    }
    
    // 解析请求体
    // 查找头部和body之间的空行分隔符
    size_t headerEnd = rawRequest.find("\r\n\r\n");
    size_t bodyStart;
    if (headerEnd != std::string::npos) {
        headerEnd_ = headerEnd;
        headerEndSepLen_ = 4;
        bodyStart = headerEnd + 4;  // 跳过 "\r\n\r\n"
    } else {
        headerEnd = rawRequest.find("\n\n");
        if (headerEnd != std::string::npos) {
            headerEnd_ = headerEnd;
            headerEndSepLen_ = 2;
            bodyStart = headerEnd + 2;  // 跳过 "\n\n"
        } else {
            // 缺少头部结束空行：请求尚不完整（如 TCP 半包），不应视为解析成功——
            // 否则调用方无法判断"需要继续读"，长连接下会反复响应同一段数据
            return false;
        }
    }

    if (bodyStart != std::string::npos) {
        body_ = rawRequest.substr(bodyStart);
    } else {
        body_ = "";
    }

    // 记录原始 body 大小（chunked 解码前）
    rawBodySize_ = body_.size();
    bodyConsumed_ = 0;

    // 检测 Transfer-Encoding: chunked
    bool isChunked = (getHeader("transfer-encoding").find("chunked") != std::string::npos);

    // RFC 9112 §6.1：Content-Length 与 Transfer-Encoding 同时出现时必须拒绝。
    // 若不拒绝，两端可能用不同的框架解析同一段字节（请求走私面）（M3）
    if (isChunked && hasHeader("content-length")) {
        conflictingFraming_ = true;
        body_.clear();
        bodyConsumed_ = 0;
        return false;
    }

    if (isChunked) {
        // 结束标记必须存在（解码前的原始报文中，从头找到第一个 "0\r\n\r\n"）
        size_t endPos = rawRequest.find("0\r\n\r\n", bodyStart);
        if (endPos == std::string::npos) {
            return false;  // 未收齐
        }
        // chunked 的 body 边界跨越整个 chunked 报文（长度行 + 数据 + 终止块），
        // 到 "0\r\n\r\n" 结束——其后的字节属于同一连接的下一个请求。
        bodyConsumed_ = endPos + 5 - bodyStart;

        if (!decodeChunkedBody()) {
            return false;
        }
    } else {
        // Content-Length 校验：body 严格按 Content-Length 切分。
        // 头部之后可能紧跟同一连接的下一个请求（管线化 / 粘包），
        // 不切分就会把后续请求的原始字节当成业务 body 交给 handler（C5）。
        contentLength_ = 0;
        contentLengthValid_ = true;
        if (hasHeader("content-length")) {
            const std::string& clRaw = getHeader("content-length");
            // 必须是非空纯十进制数字，且不溢出 size_t；
            // 容忍首尾空白。畸形值显式标为非法（由调用方回 400），不能当 0。
            std::string cl = clRaw;
            cl.erase(0, cl.find_first_not_of(" \t"));
            if (cl.empty()) {
                contentLengthValid_ = false;
            } else {
                cl.erase(cl.find_last_not_of(" \t") + 1);
                bool digitsOnly = !cl.empty() &&
                    cl.find_first_not_of("0123456789") == std::string::npos;
                if (!digitsOnly) {
                    contentLengthValid_ = false;
                } else {
                    try {
                        unsigned long long v = std::stoull(cl);
                        if (v > std::numeric_limits<size_t>::max()) {
                            contentLengthValid_ = false;
                        } else {
                            contentLength_ = static_cast<size_t>(v);
                        }
                    } catch (const std::exception&) {
                        contentLengthValid_ = false;
                    }
                }
            }
            if (!contentLengthValid_) {
                body_.clear();
                bodyConsumed_ = 0;
                return false;
            }
            // 上界检查：无上界的 Content-Length 会让对端用一个数字就让服务端
            // 在缓冲区里无限等待，或让 bodyConsumed_ 越过真实缓冲区（M3）
            if (contentLength_ > maxBodySize_) {
                bodyTooLarge_ = true;
                body_.clear();
                bodyConsumed_ = 0;
                return false;
            }
        }

        const size_t contentLength = contentLength_;
        if (contentLength == 0) {
            body_.clear();
        } else if (body_.size() < contentLength) {
            return false;  // body 未收齐
        } else {
            body_.resize(contentLength);
        }
        bodyConsumed_ = contentLength;
    }

    return true;
}

bool HttpRequest::decodeChunkedBody() {
    std::string decoded;
    size_t pos = 0;

    while (pos < body_.size()) {
        // 找到 chunk size 行
        size_t lineEnd = body_.find("\r\n", pos);
        if (lineEnd == std::string::npos) return false;

        // 解析 chunk size（hex），去除可能的 chunk extension
        std::string sizeStr = body_.substr(pos, lineEnd - pos);
        size_t semiPos = sizeStr.find(';');
        if (semiPos != std::string::npos) {
            sizeStr = sizeStr.substr(0, semiPos);
        }

        char* end;
        long chunkSize = std::strtol(sizeStr.c_str(), &end, 16);
        if (*end != '\0') return false;

        pos = lineEnd + 2;  // 跳过 \r\n

        if (chunkSize == 0) {
            break;  // 最后一个 chunk
        }

        if (pos + chunkSize + 2 > body_.size()) return false;

        decoded.append(body_.substr(pos, chunkSize));
        pos += chunkSize + 2;  // 跳过 data + \r\n
    }

    body_ = std::move(decoded);
    return true;
}

bool HttpRequest::parseRequestLine(const std::string& line) {
    std::istringstream stream(line);
    std::string method, path, version;
    
    if (!(stream >> method >> path >> version)) {
        return false;
    }
    
    method_ = stringToMethod(method);
    methodString_ = method;
    version_ = version;
    
    // 解析路径和查询参数。
    // 路径按 RFC 3986 做百分号解码后再交给路由（%2F 保留原样），
    // 这样 /users/a%20b 能匹配 /users/:id 并拿到 "a b" 而不是 "%20"（M11）
    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos) {
        path_ = urlDecode(path.substr(0, queryPos), /*skipEncodedSlash=*/true,
                          /*plusAsSpace=*/false);
        std::string queryString = path.substr(queryPos + 1);
        parseQueryParams(queryString);
    } else {
        path_ = urlDecode(path, /*skipEncodedSlash=*/true, /*plusAsSpace=*/false);
    }
    
    return true;
}

bool HttpRequest::parseHeaders(const std::vector<std::string>& headerLines) {
    for (const auto& line : headerLines) {
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }
        
        std::string name = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);
        
        // 去除前后空格
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        headers_[toLowerCase(name)] = value;
    }
    
    return true;
}

void HttpRequest::parseQueryParams(const std::string& queryString) {
    std::istringstream stream(queryString);
    std::string param;
    
    while (std::getline(stream, param, '&')) {
        size_t equalPos = param.find('=');
        if (equalPos != std::string::npos) {
            std::string name = param.substr(0, equalPos);
            std::string value = param.substr(equalPos + 1);
            queries_[urlDecode(name)] = urlDecode(value);
        } else {
            queries_[urlDecode(param)] = "";
        }
    }
}

HttpMethod HttpRequest::stringToMethod(const std::string& method) {
    std::string upperMethod = method;
    // ::toupper 传负值 char 是 UB（非 ASCII 字节可触发）→ 先转 unsigned char（M11）
    std::transform(upperMethod.begin(), upperMethod.end(), upperMethod.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    
    if (upperMethod == "GET") return HttpMethod::GET;
    if (upperMethod == "POST") return HttpMethod::POST;
    if (upperMethod == "PUT") return HttpMethod::PUT;
    if (upperMethod == "DELETE") return HttpMethod::DELETE;
    if (upperMethod == "PATCH") return HttpMethod::PATCH;
    if (upperMethod == "HEAD") return HttpMethod::HEAD;
    if (upperMethod == "OPTIONS") return HttpMethod::OPTIONS;
    
    return HttpMethod::UNKNOWN;
}

std::string HttpRequest::toLowerCase(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// %XX 解码。严格化（M11）：
//   - 必须是恰好两位十六进制，否则原样保留（旧实现用 strtol，接受 "+1"、前导空白与
//     符号，"%+1" 会被解成 0x01）；
//   - skipEncodedSlash：路径解码时保留 %2F 原样，避免编码斜杠改变路由分段语义
std::string HttpRequest::urlDecode(const std::string& str, bool skipEncodedSlash,
                                   bool plusAsSpace) {
    static const char* kHex = "0123456789abcdefABCDEF";
    std::string result;
    result.reserve(str.length());

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length() &&
            strchr(kHex, str[i + 1]) && strchr(kHex, str[i + 2])) {
            int value = (hexVal(str[i + 1]) << 4) | hexVal(str[i + 2]);
            if (skipEncodedSlash && value == '/') {
                result.append(str, i, 3);   // 保留 %2F 原样
            } else {
                result += static_cast<char>(value);
            }
            i += 2;
        } else if (str[i] == '+' && plusAsSpace) {
            result += ' ';
        } else {
            result += str[i];
        }
    }

    return result;
}

const std::string& HttpRequest::getHeader(const std::string& name) const {
    static const std::string empty;
    auto it = headers_.find(toLowerCase(name));
    return (it != headers_.end()) ? it->second : empty;
}

bool HttpRequest::hasHeader(const std::string& name) const {
    return headers_.find(toLowerCase(name)) != headers_.end();
}

const std::string& HttpRequest::getQuery(const std::string& name) const {
    static const std::string empty;
    auto it = queries_.find(name);
    return (it != queries_.end()) ? it->second : empty;
}

const std::string& HttpRequest::getParam(const std::string& name) const {
    static const std::string empty;
    auto it = params_.find(name);
    return (it != params_.end()) ? it->second : empty;
}

void HttpRequest::setParam(const std::string& name, const std::string& value) {
    params_[name] = value;
}

bool HttpRequest::isKeepAlive() const {
    // 连接复用意愿按 HTTP 版本判断：
    //   HTTP/1.1 默认长连接，仅当显式 Connection: close 时关闭
    //   HTTP/1.0 默认短连接，需显式 Connection: keep-alive
    //   其他/未知版本按短连接保守处理
    const std::string connection = toLowerCase(getHeader("connection"));
    if (version_ == "HTTP/1.1") return connection != "close";
    if (version_ == "HTTP/1.0") return connection == "keep-alive";
    return false;
}

size_t HttpRequest::getContentLength() const {
    return contentLength_;
}

std::string HttpRequest::getContentType() const {
    return getHeader("content-type");
}

void HttpRequest::setUserData(const std::string& key, const std::string& value) {
    userData_[key] = value;
}

const std::string& HttpRequest::getUserData(const std::string& key) const {
    static const std::string empty;
    auto it = userData_.find(key);
    return (it != userData_.end()) ? it->second : empty;
}

bool HttpRequest::hasUserData(const std::string& key) const {
    return userData_.find(key) != userData_.end();
}

} // namespace http
