#pragma once

// OpenSSL 初始化辅助接口
// 移植自 WebsocketServer，配置改为 TlsConfig 结构体

#include "WssTypes.h"

#include <openssl/ssl.h>
#include <string>

namespace http {
namespace wss {

// 创建服务端 SSL_CTX，加载 PEM 证书和私钥
SSL_CTX* createServerContext(const TlsConfig& cfg);

// 在已有 SSL_CTX 上创建客户端 SSL 会话对象
SSL* createClientSSL(SSL_CTX* ctx);

}  // namespace wss
}  // namespace http
