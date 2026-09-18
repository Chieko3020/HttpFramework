// TLS 上下文构建实现
// 移植自 WebsocketServer，环境变量改为 TlsConfig 结构体

#include "HttpFramework/wss/OpenSslHelpers.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace http {
namespace wss {

namespace {

// 统一的失败出口：记录 OpenSSL 错误队列并释放已分配的 SSL_CTX。
// createServerContext 的错误语义是"返回 nullptr + 打印原因"，不抛异常——
// 调用方（WssReactor::start / test_edge_cert）都以 nullptr 作为失败判据。
SSL_CTX* failContext(SSL_CTX* ctx, const char* what) {
    unsigned long err = ERR_get_error();
    char buf[256];
    if (err != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
    } else {
        std::snprintf(buf, sizeof(buf), "%s", "no OpenSSL error queued");
    }
    std::cerr << "[ERROR][WSS-TLS]：" << what << ": " << buf << std::endl;
    if (ctx) {
        SSL_CTX_free(ctx);
    }
    return nullptr;
}

}  // namespace

SSL_CTX* createServerContext(const TlsConfig& cfg) {
    std::cout << "[INFO][WSS-TLS]：创建TLS上下文, minTls=" << cfg.minTlsVersion << std::endl;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return failContext(nullptr, "SSL_CTX_new failed");

    if (cfg.minTlsVersion <= 12) {
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    }

    // 空路径直接判失败：文件名校验交给 OpenSSL 会拿到语焉不详的 ENOENT，
    // 且不同版本对空字符串的处理不一致。
    if (cfg.certFile.empty()) {
        return failContext(ctx, "certificate file path is empty");
    }
    if (cfg.keyFile.empty()) {
        return failContext(ctx, "private key file path is empty");
    }

    if (SSL_CTX_use_certificate_file(ctx, cfg.certFile.c_str(), SSL_FILETYPE_PEM) <= 0)
        return failContext(ctx, "load certificate failed");
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg.keyFile.c_str(), SSL_FILETYPE_PEM) <= 0)
        return failContext(ctx, "load private key failed");
    if (!SSL_CTX_check_private_key(ctx))
        return failContext(ctx, "private key check failed");

    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);

    long timeout = cfg.sessionTimeoutSeconds > 0 ? cfg.sessionTimeoutSeconds : 300;
    SSL_CTX_set_timeout(ctx, timeout);

    if (!cfg.enableSessionTicket)
        SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);

    if (cfg.enable0Rtt) {
        SSL_CTX_set_max_early_data(ctx, cfg.maxEarlyData);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
        // 保留 OpenSSL 内建 anti-replay：关闭它会让捕获到的 0-RTT 记录可原样重放，
        // 而应用层的 nonce 校验默认并不启用（见 WebSocketCodec 的 shouldEnforceNonce）。
        std::cout << "[INFO][WSS-TLS]：0-RTT已启用, max_early_data="
                  << cfg.maxEarlyData << ", anti_replay=ON" << std::endl;
#endif
    } else {
        SSL_CTX_set_max_early_data(ctx, 0);
    }

    std::cout << "[INFO][WSS-TLS]：TLS上下文创建完成, ticket="
              << (cfg.enableSessionTicket ? "on" : "off")
              << ", timeout=" << timeout << "s" << std::endl;
    return ctx;
}

SSL* createClientSSL(SSL_CTX* ctx) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) throw std::runtime_error("SSL_new failed");
    SSL_set_connect_state(ssl);
    return ssl;
}

}  // namespace wss
}  // namespace http
