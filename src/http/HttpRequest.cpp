#include "http/HttpRequest.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

namespace http {

HttpRequest::HttpRequest() 
    : method_(HttpMethod::UNKNOWN), methodString_(""), path_(""), version_("") {
}

bool HttpRequest::parse(const std::string& rawRequest) {
    rawRequest_ = rawRequest;
    
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
    if (headerEnd == std::string::npos) {
        headerEnd = rawRequest.find("\n\n");
    }
    
    if (headerEnd != std::string::npos) {
        // 提取body部分
        body_ = rawRequest.substr(headerEnd + 4); // 跳过 "\r\n\r\n" 或 "\n\n"
        
        // 移除body末尾的换行符
        while (!body_.empty() && (body_.back() == '\r' || body_.back() == '\n')) {
            body_.pop_back();
        }
    } else {
        body_ = "";
    }
    
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
    
    // 解析路径和查询参数
    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos) {
        path_ = path.substr(0, queryPos);
        std::string queryString = path.substr(queryPos + 1);
        parseQueryParams(queryString);
    } else {
        path_ = path;
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
    std::transform(upperMethod.begin(), upperMethod.end(), upperMethod.begin(), ::toupper);
    
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
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string HttpRequest::urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            std::string hex = str.substr(i + 1, 2);
            char* end;
            long value = std::strtol(hex.c_str(), &end, 16);
            if (*end == '\0') {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
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
    std::string connection = getHeader("connection");
    return toLowerCase(connection) == "keep-alive";
}

size_t HttpRequest::getContentLength() const {
    std::string contentLength = getHeader("content-length");
    if (contentLength.empty()) {
        return 0;
    }
    
    try {
        return std::stoul(contentLength);
    } catch (const std::exception&) {
        return 0;
    }
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
