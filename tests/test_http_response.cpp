// test_http_response.cpp — HttpResponse 响应构建单元测试
// 验证：setHtml/setJson/setText Content-Type、setStatus 状态码、
//       setFile 文件加载、toString 序列化、redirect、clear

#include "http/HttpResponse.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static bool test_set_html_content_type() {
    TEST("setHtml 设置 Content-Type 为 text/html");
    http::HttpResponse res;
    res.setHtml("<h1>你好</h1>");

    CHECK(res.getHeader("Content-Type").find("text/html") != std::string::npos,
          "期望 text/html, 实际 '" << res.getHeader("Content-Type") << "'");
    CHECK(res.getBody() == "<h1>你好</h1>", "响应体不匹配");

    PASS();
    return true;
}

static bool test_set_json_content_type() {
    TEST("setJson 设置 Content-Type 为 application/json");
    http::HttpResponse res;
    res.setJson("{\"status\":\"ok\"}");

    CHECK(res.getHeader("Content-Type").find("application/json") != std::string::npos,
          "期望 application/json, 实际 '" << res.getHeader("Content-Type") << "'");
    CHECK(res.getBody() == "{\"status\":\"ok\"}", "响应体不匹配");

    PASS();
    return true;
}

static bool test_set_text_content_type() {
    TEST("setText 设置 Content-Type 为 text/plain");
    http::HttpResponse res;
    res.setText("纯文本消息");

    CHECK(res.getHeader("Content-Type").find("text/plain") != std::string::npos,
          "期望 text/plain, 实际 '" << res.getHeader("Content-Type") << "'");

    PASS();
    return true;
}

static bool test_set_status_codes() {
    TEST("setStatus 设置正确的状态码");
    http::HttpResponse res;

    res.setStatus(http::HttpStatus::OK);
    CHECK(res.getStatusCode() == 200, "期望 200");

    res.setStatus(http::HttpStatus::NOT_FOUND);
    CHECK(res.getStatusCode() == 404, "期望 404");

    res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
    CHECK(res.getStatusCode() == 500, "期望 500");

    // 自定义状态码
    res.setStatus(418);
    CHECK(res.getStatusCode() == 418, "期望 418");

    PASS();
    return true;
}

static bool test_set_status_with_reason() {
    TEST("setStatus 支持自定义原因短语");
    http::HttpResponse res;
    res.setStatus(429, "Too Many Requests");

    CHECK(res.getStatusCode() == 429, "期望 429");
    CHECK(res.getReasonPhrase() == "Too Many Requests", "原因短语不匹配");

    PASS();
    return true;
}

static bool test_set_file_disk_load() {
    TEST("setFile 从磁盘加载文件内容");
    std::string tmpPath = "/tmp/test_response_file.txt";
    {
        std::ofstream f(tmpPath);
        f << "hello from file" << std::endl;
    }

    http::HttpResponse res;
    res.setFile(tmpPath);

    CHECK(res.getBody() == "hello from file\n", "文件内容不匹配: '" << res.getBody() << "'");
    CHECK(!res.getHeader("Content-Type").empty(),
          "应设置 Content-Type, 实际 '" << res.getHeader("Content-Type") << "'");

    std::remove(tmpPath.c_str());

    PASS();
    return true;
}

static bool test_set_file_nonexistent() {
    TEST("setFile 对不存在的文件返回 404");
    http::HttpResponse res;
    res.setFile("/tmp/nonexistent_file_xyz123.txt");

    CHECK(res.getStatusCode() == 404, "期望 404, 实际 " << res.getStatusCode());

    PASS();
    return true;
}

static bool test_string_to_string_format() {
    TEST("toString 生成合法的 HTTP 响应");
    http::HttpResponse res;
    res.setStatus(http::HttpStatus::OK);
    res.setHtml("<p>test</p>");

    std::string http = res.toString();

    CHECK(http.find("HTTP/1.1 200 OK") != std::string::npos, "状态行缺失");
    CHECK(http.find("Content-Type: text/html") != std::string::npos, "Content-Type 头缺失");
    CHECK(http.find("<p>test</p>") != std::string::npos, "响应体缺失");
    CHECK(http.find("\r\n\r\n") != std::string::npos, "头体分隔符缺失");

    PASS();
    return true;
}

static bool test_redirect() {
    TEST("redirect 设置 Location 头和 302 状态码");
    http::HttpResponse res;
    res.redirect("/new-location");

    CHECK(res.getStatusCode() == 302, "期望 302, 实际 " << res.getStatusCode());
    CHECK(res.getHeader("Location") == "/new-location", "Location 头缺失");

    res.redirect("/permanent", http::HttpStatus::FOUND);
    PASS();
    return true;
}

static bool test_custom_headers() {
    TEST("setHeader 和 getHeader 正常工作");
    http::HttpResponse res;
    res.setHeader("X-Custom", "my-value");
    res.setHeader("X-Request-Id", "abc-123");

    CHECK(res.getHeader("X-Custom") == "my-value", "自定义头不匹配");
    CHECK(res.getHeader("X-Request-Id") == "abc-123", "请求 id 头不匹配");
    CHECK(res.getHeader("X-Nonexistent") == "", "不存在头应返回空串");

    PASS();
    return true;
}

static bool test_clear() {
    TEST("clear 将响应重置为初始状态");
    http::HttpResponse res;
    res.setStatus(404);
    res.setHtml("<h1>Error</h1>");
    res.setHeader("X-Test", "value");

    res.clear();

    CHECK(res.getStatusCode() == 200, "状态码应重置为 200, 实际 " << res.getStatusCode());
    CHECK(res.getBody() == "", "响应体应清空");
    CHECK(res.getHeader("X-Test") == "", "自定义头应清除");

    PASS();
    return true;
}

static bool test_set_body_binary() {
    TEST("setBody 支持带长度的二进制数据");
    http::HttpResponse res;
    const char* data = "binary\x00data";
    // 通过显式长度传递包含 '\0' 的 11 字节数据
    res.setBody(data, 11);

    std::string body = res.getBody();
    // std::string 支持内嵌 '\0'，length() 应返回 11
    CHECK(body.size() == 11, "期望 body 长度 11, 实际 " << body.size());

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_response ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_set_html_content_type,    "setHtml Content-Type");
    run(test_set_json_content_type,    "setJson Content-Type");
    run(test_set_text_content_type,    "setText Content-Type");
    run(test_set_status_codes,         "setStatus 状态码");
    run(test_set_status_with_reason,   "setStatus 原因短语");
    run(test_set_file_disk_load,       "setFile 文件加载");
    run(test_set_file_nonexistent,     "setFile 文件不存在");
    run(test_string_to_string_format,  "toString 序列化");
    run(test_redirect,                 "redirect 重定向");
    run(test_custom_headers,           "自定义头");
    run(test_clear,                    "clear 重置");
    run(test_set_body_binary,          "setBody 二进制");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
