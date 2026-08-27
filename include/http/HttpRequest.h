#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace http {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    UNKNOWN
};

class HttpRequest {
public:
    HttpRequest();
    ~HttpRequest() = default;
    
    // 解析HTTP请求
    bool parse(const std::string& rawRequest);
    
    // 获取方法
    HttpMethod getMethod() const { return method_; }
    const std::string& getMethodString() const { return methodString_; }
    
    // 获取路径
    const std::string& getPath() const { return path_; }
    
    // 获取版本
    const std::string& getVersion() const { return version_; }
    
    // 获取头部
    const std::string& getHeader(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& getHeaders() const { return headers_; }
    
    // 获取查询参数
    const std::string& getQuery(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& getQueries() const { return queries_; }
    
    // 获取路径参数
    const std::string& getParam(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& getParams() const { return params_; }
    
    // 获取请求体
    const std::string& getBody() const { return body_; }
    
    // 获取原始请求
    const std::string& getRawRequest() const { return rawRequest_; }
    
    // 设置路径参数（由路由系统使用）
    void setParam(const std::string& name, const std::string& value);
    
    // 检查是否为keep-alive连接
    bool isKeepAlive() const;
    
    // 获取Content-Length
    size_t getContentLength() const;
    
    // 获取头部结束位置（用于计算已消费数据量，解决粘包问题）
    size_t getHeaderEnd() const { return headerEnd_; }
    size_t getHeaderEndSepLen() const { return headerEndSepLen_; }
    size_t getRawBodySize() const { return rawBodySize_; }  // chunked 解码前原始 body 大小
    
    // 获取Content-Type
    std::string getContentType() const;
    
    // 用户数据管理
    void setUserData(const std::string& key, const std::string& value);
    const std::string& getUserData(const std::string& key) const;
    bool hasUserData(const std::string& key) const;

private:
    HttpMethod method_;
    std::string methodString_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::unordered_map<std::string, std::string> queries_;
    std::unordered_map<std::string, std::string> params_;
    std::unordered_map<std::string, std::string> userData_;
    std::string body_;
    std::string rawRequest_;
    size_t headerEnd_ = 0;
    size_t headerEndSepLen_ = 0;  // 分隔符长度：\r\n\r\n=4, \n\n=2
    size_t rawBodySize_ = 0;      // chunked 解码前原始 body 大小
    
    // 解析请求行
    bool parseRequestLine(const std::string& line);
    
    // 解析头部
    bool parseHeaders(const std::vector<std::string>& headerLines);
    
    // 解析查询参数
    void parseQueryParams(const std::string& queryString);
    
    // 字符串转HTTP方法
    HttpMethod stringToMethod(const std::string& method);
    
    // 字符串转小写
    static std::string toLowerCase(const std::string& str);
    
    // URL解码
    std::string urlDecode(const std::string& str);
    
    // chunked transfer encoding 解码
    bool decodeChunkedBody();
};

} // namespace http
