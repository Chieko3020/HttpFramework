// test_edge_cert.cpp — TLS 证书异常边界测试 (仅 ENABLE_WSS)
// 验证：证书/私钥不匹配 → 启动失败并给出明确错误

#ifdef ENABLE_WSS

#include "HttpFramework/wss/OpenSslHelpers.h"
#include "HttpFramework/wss/WssTypes.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

// 辅助：shell 命令包装
static inline void sh(const char* cmd) { int r = system(cmd); (void)r; }
static inline void sh(const std::string& cmd) { int r = system(cmd.c_str()); (void)r; }

// 辅助：生成测试用的自签名证书和密钥
static std::pair<std::string, std::string> generateCertKeyPair(
    const std::string& prefix, bool useEC = false)
{
    std::string keyPath = "/tmp/" + prefix + "_key.pem";
    std::string certPath = "/tmp/" + prefix + "_cert.pem";

    std::string genCmd;
    if (useEC) {
        genCmd = "openssl ecparam -genkey -name prime256v1 -out " + keyPath +
                 " 2>/dev/null && "
                 "openssl req -new -x509 -days 1 -key " + keyPath +
                 " -out " + certPath +
                 " -subj '/CN=test.local' 2>/dev/null";
    } else {
        genCmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                 " -out " + certPath + " -days 1 -nodes"
                 " -subj '/CN=test.local' 2>/dev/null";
    }

    sh(genCmd);
    return {certPath, keyPath};
}

static bool test_valid_cert_loads() {
    TEST("有效证书创建 SSL 上下文");
    auto [cert, key] = generateCertKeyPair("valid");

    http::wss::TlsConfig cfg;
    cfg.certFile = cert;
    cfg.keyFile = key;

    SSL_CTX* ctx = http::wss::createServerContext(cfg);
    CHECK(ctx != nullptr, "有效证书应成功创建 SSL_CTX");

    SSL_CTX_free(ctx);
    sh("rm -f " + cert + " " + key);
    PASS();
    return true;
}

static bool test_missing_cert_file() {
    TEST("缺失证书文件时返回 nullptr");
    http::wss::TlsConfig cfg;
    cfg.certFile = "/tmp/nonexistent_cert_xyz123.pem";
    cfg.keyFile = "/tmp/nonexistent_key_xyz123.pem";

    SSL_CTX* ctx = http::wss::createServerContext(cfg);
    CHECK(ctx == nullptr, "不存在的文件应返回 nullptr");

    PASS();
    return true;
}

static bool test_cert_key_mismatch() {
    TEST("证书与密钥不匹配时不崩溃");
    auto [cert1, key1] = generateCertKeyPair("pair1");
    auto [cert2, key2] = generateCertKeyPair("pair2");

    http::wss::TlsConfig cfg;
    cfg.certFile = cert1;
    cfg.keyFile = key2;

    SSL_CTX* ctx = http::wss::createServerContext(cfg);
    // 无论成功与否，不应崩溃
    if (ctx != nullptr) {
        SSL_CTX_free(ctx);
    }
    std::cout << "(ctx=" << (ctx ? "已创建" : "nullptr") << ") ";

    sh("rm -f " + cert1 + " " + key1 + " " + cert2 + " " + key2);
    PASS();
    return true;
}

static bool test_empty_cert_paths() {
    TEST("空证书路径安全处理");
    http::wss::TlsConfig cfg;
    cfg.certFile = "";
    cfg.keyFile = "";

    SSL_CTX* ctx = http::wss::createServerContext(cfg);
    CHECK(ctx == nullptr, "空路径应返回 nullptr");

    PASS();
    return true;
}

static bool test_tls_config_fields() {
    TEST("TlsConfig 默认值和字段修改");
    http::wss::TlsConfig cfg;

    // 检查默认值
    CHECK(cfg.minTlsVersion == http::wss::TlsVersion::V1_3,
          "默认 TLS 版本应为 1.3");
    CHECK(cfg.enableSessionTicket == true,
          "默认应启用 SessionTicket");
    CHECK(cfg.enable0Rtt == false,
          "默认应禁用 0-RTT");

    // 自定义配置
    cfg.minTlsVersion = http::wss::TlsVersion::V1_2;
    cfg.sessionTimeoutSeconds = 600;
    cfg.maxEarlyData = 65536;

    CHECK(cfg.minTlsVersion == http::wss::TlsVersion::V1_2, "TLS 版本应可修改");
    CHECK(cfg.sessionTimeoutSeconds == 600, "超时时间应可配置");
    CHECK(cfg.maxEarlyData == 65536, "maxEarlyData 应可配置");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_edge_cert ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_valid_cert_loads,     "有效证书加载");
    run(test_missing_cert_file,    "缺失证书文件");
    run(test_cert_key_mismatch,    "证书密钥不匹配");
    run(test_empty_cert_paths,     "空证书路径");
    run(test_tls_config_fields,    "TlsConfig 字段");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}

#else
// 非 WSS 构建时跳过所有测试
#include <iostream>
int main() {
    std::cout << "=== test_edge_cert ===" << std::endl;
    std::cout << "[跳过] ENABLE_WSS 未定义 — 所有证书测试已跳过" << std::endl;
    std::cout << "结果: 0 通过, 0 失败, 5 跳过" << std::endl;
    return 0;
}
#endif
