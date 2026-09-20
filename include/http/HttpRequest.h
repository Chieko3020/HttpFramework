#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace http {

// chunked 报文扫描结论（L13）
enum class ChunkScanResult {
    Complete,    // 已收齐（含 trailer）
    Incomplete,  // 数据还不够
    Malformed,   // chunk size 非法（非十六进制/超 16 位）
    TooLarge     // 累计超过 maxBodySize_
};

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
    
    // 解析HTTP请求。
    // 接受 string_view：调用方可以直接把连接缓冲的视图传进来，省掉一次整块拷贝（M1）。
    // 内部只在开头把视图拷进 rawRequest_（解析需要稳定存储）。
    bool parse(std::string_view rawRequest);
    
    // 获取方法
    HttpMethod getMethod() const { return method_; }
    const std::string& getMethodString() const { return methodString_; }
    
    // 获取路径
    const std::string& getPath() const { return path_; }
    
    // 获取版本
    const std::string& getVersion() const { return version_; }
    
    // 获取头部
    const std::string& getHeader(const std::string& name) const;
    bool hasHeader(const std::string& name) const;
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
    // Content-Length 头是否语法合法（缺省视为合法=0）。
    // 畸形值（非纯数字 / 溢出）必须被显式拒绝，否则会被当作 0 处理，
    // 让该请求的 body 被解析成"下一个请求"，构成请求走私面。
    bool isContentLengthValid() const { return contentLengthValid_; }

    // 请求头自相矛盾（Content-Length 与 Transfer-Encoding 同时存在）：
    // 按 RFC 9112 §6.1 必须拒绝，否则两端对报文边界的理解可能不一致 → 请求走私面（M3）
    bool hasConflictingFraming() const { return conflictingFraming_; }

    // 请求语法畸形（请求行缺字段/版本非法、头部行没有冒号等）：
    // 这类请求"永远不会变得合法"，必须显式回 400，而不是被当成"还没收齐"
    // 一直等到空闲超时（L3）
    bool isMalformed() const { return malformed_; }

    // 请求体上限（可由 HttpServer 配置）：超过则视为不可接受（回 413 而不是当作 0 处理）
    void setMaxBodySize(size_t maxBytes) { maxBodySize_ = maxBytes; }
    size_t getMaxBodySize() const { return maxBodySize_; }
    bool isBodyTooLarge() const { return bodyTooLarge_; }
    
    // 获取头部结束位置（用于计算已消费数据量，解决粘包问题）
    size_t getHeaderEnd() const { return headerEnd_; }
    size_t getHeaderEndSepLen() const { return headerEndSepLen_; }
    // chunked 解码前原始 body 大小（仅 chunked 有意义，供诊断/统计）
    size_t getRawBodySize() const { return rawBodySize_; }
    // 本次请求的 body 在原始缓冲区中占用的字节数：
    //   非 chunked：Content-Length（无该头时为 0）
    //   chunked  ：整个 chunked 报文（含长度行与终止块）的字节数
    // 调用方据此推进解析偏移，实现管线化 / 粘包的正确切分。
    size_t getBodyConsumed() const { return bodyConsumed_; }
    
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
    size_t bodyConsumed_ = 0;     // 本次请求在原始缓冲区中占用 body 的字节数
    size_t contentLength_ = 0;    // 解析后的 Content-Length
    bool contentLengthValid_ = true;  // Content-Length 语法是否合法
    bool conflictingFraming_ = false; // CL 与 TE 同时存在
    bool malformed_ = false;          // 语法畸形（L3）
    bool bodyTooLarge_ = false;       // 超过 maxBodySize_
    size_t maxBodySize_ = 64u * 1024u * 1024u;  // 默认 64MB 上限（M3）
    
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
    
    // URL解码（M11）：
    //   skipEncodedSlash=true —— 保留 %2F 原样（路径解码用，避免编码斜杠改变分段语义）
    //   plusAsSpace=true      —— 把 '+' 解成空格（application/x-www-form-urlencoded 的
    //                            查询串语义；路径里的 '+' 是字面量，不做替换）
    std::string urlDecode(const std::string& str, bool skipEncodedSlash = false,
                          bool plusAsSpace = true);
    
    // chunked transfer encoding 解码
    bool decodeChunkedBody();
    // 扫描 chunked 报文边界（含 trailer 处理，L13）
    ChunkScanResult scanChunkedEnd(const std::string& raw, size_t bodyStart, size_t* endOff);
};

} // namespace http
