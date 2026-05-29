// TLS 上下文构建实现
// 移植自 WebsocketServer，环境变量改为 TlsConfig 结构体

#include "HttpFramework/wss/OpenSslHelpers.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace http {
namespace wss {

namespace {

void throwOnOpenSslError(const char* what) {
    unsigned long err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
}

}  // namespace

SSL_CTX* createServerContext(const TlsConfig& cfg) {
    std::cout << "[INFO][WSS-TLS]：创建TLS上下文, minTls=" << cfg.minTlsVersion << std::endl;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) throwOnOpenSslError("SSL_CTX_new failed");

    if (cfg.minTlsVersion <= 12) {
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    }

    if (SSL_CTX_use_certificate_file(ctx, cfg.certFile.c_str(), SSL_FILETYPE_PEM) <= 0)
        throwOnOpenSslError("load certificate failed");
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg.keyFile.c_str(), SSL_FILETYPE_PEM) <= 0)
        throwOnOpenSslError("load private key failed");
    if (!SSL_CTX_check_private_key(ctx))
        throw std::runtime_error("private key check failed");

    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);

    long timeout = cfg.sessionTimeoutSeconds > 0 ? cfg.sessionTimeoutSeconds : 300;
    SSL_CTX_set_timeout(ctx, timeout);

    if (!cfg.enableSessionTicket)
        SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);

    if (cfg.enable0Rtt) {
        SSL_CTX_set_max_early_data(ctx, cfg.maxEarlyData);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
        SSL_CTX_set_options(ctx, SSL_OP_NO_ANTI_REPLAY);
        std::cout << "[INFO][WSS-TLS]：0-RTT已启用, max_early_data="
                  << cfg.maxEarlyData << ", anti_replay=OFF)" << std::endl;
#endif
    } else {
        SSL_CTX_set_max_early_data(ctx, 0);
    }

    std::cout << "[INFO][WSS-TLS]：TLS上下文创建完成, ticket="
              << (cfg.enableSessionTicket ? "on" : "off")
              << ", timeout=" << timeout << "s)" << std::endl;
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
