#include "http/HttpServer.h"
#include "router/Router.h"
#include "session/SessionManager.h"
#include "middleware/SessionMiddleware.h"
#include "utils/db/DbConnectionPool.h"
#include "utils/TemplateLoader.h"

#include <iostream>
#include <signal.h>
#include <memory>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>

// 全局服务器和资源指针
std::shared_ptr<http::HttpServer> g_httpServer;
std::shared_ptr<session::SessionManager> g_sessionManager;
std::shared_ptr<db::DbConnectionPool> g_dbPool;

void signalHandler(int signal) {
    static bool shutdownInProgress = false;
    
    if (shutdownInProgress) {
        std::cout << "\nForce shutdown..." << std::endl;
        _exit(1);  // 使用_exit避免再次调用析构函数
    }
    
    shutdownInProgress = true;
    std::cout << "\nReceived signal " << signal << ", shutting down servers..." << std::endl;
    
    // 设置一个定时器，如果5秒内没有完成关闭，强制退出
    std::thread([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cout << "\nShutdown timeout, force exit..." << std::endl;
        _exit(1);
    }).detach();
    
    try {
        // 按顺序关闭服务器
        if (g_httpServer) {
            std::cout << "Stopping HTTP server..." << std::endl;
            g_httpServer->stop();
        }
        
        
        if (g_sessionManager) {
            std::cout << "Stopping session manager..." << std::endl;
            g_sessionManager->stopCleanupThread();
        }
        
        // 异步关闭数据库连接池，避免阻塞
        if (g_dbPool) {
            std::cout << "Shutting down database pool..." << std::endl;
            std::thread([&]() {
                try {
                    g_dbPool->shutdown();
                } catch (const std::exception& e) {
                    std::cerr << "Database shutdown error: " << e.what() << std::endl;
                }
            }).detach();
        }
        
        std::cout << "Servers shut down gracefully." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error during shutdown: " << e.what() << std::endl;
    }
    
    exit(0);
}

// 检查端口是否被占用
bool isPortInUse(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    int result = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    
    return result != 0;
}

// 获取当前时间字符串
std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// URL解码函数
std::string urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '+') {
            result += ' ';
        } else if (str[i] == '%' && i + 2 < str.size()) {
            std::string hex = str.substr(i + 1, 2);
            char* end;
            long value = std::strtol(hex.c_str(), &end, 16);
            if (*end == '\0') {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else {
            result += str[i];
        }
    }
    
    return result;
}

// 创建路由配置
std::shared_ptr<router::Router> createRouter() {
    auto router = std::make_shared<router::Router>();
    
    // 1. 全局中间件 - 请求日志
    router->use([](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
        (void)res;
        std::cout << "[" << getCurrentTime() << "] " 
                  << req.getMethodString() << " " << req.getPath() 
                  << " - " << req.getHeader("User-Agent") << std::endl;
        next();
    });
    
    // 2. 会话中间件
    router->use(middleware::session_utils::createSessionMiddleware(g_sessionManager));
    
    // 3. 主页 - 综合功能展示
    router->get("/", [](const http::HttpRequest& req, http::HttpResponse& res) {
        (void)req;
        std::string html = utils::TemplateLoader::loadTemplate("index.html");
        res.setHtml(html);
    });
    
    // 1.1 API状态页面
    router->get("/api/status", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        
        if (accept.find("text/html") != std::string::npos) {
            // 使用模板文件
            std::map<std::string, std::string> variables;
            variables["CURRENT_TIME"] = getCurrentTime();
            
            std::string html = utils::TemplateLoader::loadTemplate("api_status.html", variables);
            res.setHtml(html);
        } else {
            // 返回JSON数据
            res.setJson(R"({
                "status": "running",
                "architecture": "Reactor + ThreadPool",
                "port": 8080,
                "version": "HttpFramework v1.0.0",
                "timestamp": ")" + getCurrentTime() + R"("
            })");
        }
    });
    
    // 1.2 性能统计页面
    router->get("/api/stats", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        
        if (accept.find("text/html") != std::string::npos) {
            if (g_httpServer) {
                const auto& stats = g_httpServer->getStatistics();
                
                // 使用模板文件
                std::map<std::string, std::string> variables;
                variables["TOTAL_REQUESTS"] = std::to_string(stats.totalRequests.load());
                variables["ACTIVE_CONNECTIONS"] = std::to_string(stats.activeConnections.load());
                variables["COMPLETED_REQUESTS"] = std::to_string(stats.completedRequests.load());
                variables["QUEUED_TASKS"] = std::to_string(stats.queuedTasks.load());
                
                std::string html = utils::TemplateLoader::loadTemplate("api_stats.html", variables);
                res.setHtml(html);
            } else {
                res.setStatus(http::HttpStatus::SERVICE_UNAVAILABLE);
                res.setHtml("<h1>服务不可用</h1><p>服务器未运行</p>");
            }
        } else {
            // 返回JSON数据
            if (g_httpServer) {
                const auto& stats = g_httpServer->getStatistics();
                res.setJson(R"({
                    "total_requests": )" + std::to_string(stats.totalRequests.load()) + R"(,
                    "active_connections": )" + std::to_string(stats.activeConnections.load()) + R"(,
                    "completed_requests": )" + std::to_string(stats.completedRequests.load()) + R"(,
                    "queued_tasks": )" + std::to_string(stats.queuedTasks.load()) + R"(,
                    "architecture": "Reactor + ThreadPool",
                    "features": ["epoll ET模式", "非阻塞I/O", "任务队列"]
                })");
            } else {
                res.setStatus(http::HttpStatus::SERVICE_UNAVAILABLE);
                res.setJson(R"({"error": "Server not available"})");
            }
        }
    });

    
    // 5. 时间端点
    router->get("/api/time", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        std::string currentTime = getCurrentTime();
        std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        if (accept.find("text/html") != std::string::npos) {
            // 使用模板文件
            std::map<std::string, std::string> variables;
            variables["CURRENT_TIME"] = currentTime;
            variables["TIMESTAMP"] = timestamp;
            
            std::string html = utils::TemplateLoader::loadTemplate("api_time.html", variables);
            res.setHtml(html);
        } else {
            // 返回JSON数据
            res.setJson(R"({
                "timestamp": )" + timestamp + R"(,
                "datetime": ")" + currentTime + R"(",
                "timezone": "local"
            })");
        }
    });
    
    // 6. Echo端点
    router->post("/api/echo", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::cout << "Echo endpoint - Content-Length: " << req.getContentLength() << std::endl;
        std::cout << "Echo endpoint - Body length: " << req.getBody().length() << std::endl;
        std::cout << "Echo endpoint - Body content: '" << req.getBody() << "'" << std::endl;
        
        res.setJson(R"({
            "method": ")" + req.getMethodString() + R"(",
            "path": ")" + req.getPath() + R"(",
            "headers": {
                "content-type": ")" + req.getHeader("Content-Type") + R"(",
                "user-agent": ")" + req.getHeader("User-Agent") + R"(",
                "content-length": ")" + std::to_string(req.getContentLength()) + R"("
            },
            "body": ")" + req.getBody() + R"(",
            "body_length": )" + std::to_string(req.getBody().length()) + R"(,
            "timestamp": ")" + getCurrentTime() + R"("
        })");
    });
    
    // 7. 会话信息端点
    router->get("/session/info", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        std::string sessionId = req.getUserData("session");
        auto session = g_sessionManager->getSession(sessionId);
        
        if (session) {
            // 获取访问计数
            std::string visitCount = "1";
            if (session->has("visit_count")) {
                visitCount = session->get("visit_count");
            }
            
            if (accept.find("text/html") != std::string::npos) {
                // 使用模板文件
                std::map<std::string, std::string> variables;
                variables["SESSION_ID"] = sessionId;
                variables["LAST_ACCESS"] = getCurrentTime();
                variables["VISIT_COUNT"] = visitCount;
                variables["USER_AGENT"] = req.getHeader("User-Agent");
                
                std::string html = utils::TemplateLoader::loadTemplate("session_info.html", variables);
                res.setHtml(html);
            } else {
                // 返回JSON数据
                res.setJson(R"({
                    "session_id": ")" + sessionId + R"(",
                    "last_access": ")" + getCurrentTime() + R"(",
                    "data": {
                        "visit_count": ")" + visitCount + R"(",
                        "user_agent": ")" + req.getHeader("User-Agent") + R"("
                    }
                })");
            }
            
            // 增加访问计数
            int count = std::stoi(visitCount) + 1;
            session->set("visit_count", std::to_string(count));
        } else {
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            if (accept.find("text/html") != std::string::npos) {
                res.setHtml("<h1>会话错误</h1><p>会话未找到</p><a href='/'>返回首页</a>");
            } else {
                res.setJson(R"({"error": "Session not found"})");
            }
        }
    });
    
    // 8. 设置会话数据
    router->post("/session/set", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string sessionId = req.getUserData("session");
        auto session = g_sessionManager->getSession(sessionId);
        
        if (session) {
            // 简单的表单解析
            std::string body = req.getBody();
            std::string key, value;
            
            size_t keyPos = body.find("key=");
            size_t valuePos = body.find("value=");
            
            if (keyPos != std::string::npos && valuePos != std::string::npos) {
                keyPos += 4;
                valuePos += 6;
                
                size_t keyEnd = body.find("&", keyPos);
                size_t valueEnd = body.find("&", valuePos);
                
                if (keyEnd == std::string::npos) keyEnd = body.length();
                if (valueEnd == std::string::npos) valueEnd = body.length();
                
                key = body.substr(keyPos, keyEnd - keyPos);
                value = body.substr(valuePos, valueEnd - valuePos);
                
                session->set(key, value);
                
                res.setJson(R"({
                    "status": "success",
                    "message": "Session data updated",
                    "key": ")" + key + R"(",
                    "value": ")" + value + R"("
                })");
            } else {
                res.setStatus(http::HttpStatus::BAD_REQUEST);
                res.setJson(R"({"error": "Invalid form data"})");
            }
        } else {
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            res.setJson(R"({"error": "Session not found"})");
        }
    });
    
    // 9. 会话统计
    router->get("/session/stats", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        auto sessions = g_sessionManager->getAllSessions();
        std::string totalSessions = std::to_string(sessions.size());
        
        if (accept.find("text/html") != std::string::npos) {
            // 使用模板文件
            std::map<std::string, std::string> variables;
            variables["TOTAL_SESSIONS"] = totalSessions;
            variables["ACTIVE_SESSIONS"] = totalSessions;
            
            std::string html = utils::TemplateLoader::loadTemplate("session_stats.html", variables);
            res.setHtml(html);
        } else {
            // 返回JSON数据
            res.setJson(R"({
                "total_sessions": )" + totalSessions + R"(,
                "active_sessions": )" + totalSessions + R"(,
                "session_timeout": 1800,
                "cleanup_interval": 300
            })");
        }
    });
    
    // 10. 数据库测试
    router->get("/db/test", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        if (!g_dbPool) {
            res.setStatus(http::HttpStatus::SERVICE_UNAVAILABLE);
            res.setJson(R"({"status": "error", "message": "Database not configured. Set up MySQL and restart."})");
            return;
        }
        auto conn = g_dbPool->getConnection();
        if (!conn) {
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            if (accept.find("text/html") != std::string::npos) {
                res.setHtml("<h1>数据库错误</h1><p>无法获取数据库连接</p><a href='/'>返回首页</a>");
            } else {
                res.setJson(R"({"status": "error", "message": "Failed to get database connection"})");
            }
            return;
        }

        try {
            auto result = conn->executeQuery("SELECT 1 as test_value");
            g_dbPool->returnConnection(conn);

            if (!result) {
                res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
                if (accept.find("text/html") != std::string::npos) {
                    res.setHtml("<h1>数据库错误</h1><p>查询失败</p><a href='/'>返回首页</a>");
                } else {
                    res.setJson(R"({"status": "error", "message": "Query failed"})");
                }
                return;
            }

            if (result->next()) {
                std::string testValue = std::to_string(result->getInt("test_value"));
                std::string totalConnections = std::to_string(g_dbPool->getTotalConnections());
                std::string availableConnections = std::to_string(g_dbPool->getAvailableConnections());
                
                if (accept.find("text/html") != std::string::npos) {
                    // 使用模板文件
                    std::map<std::string, std::string> variables;
                    variables["TEST_VALUE"] = testValue;
                    variables["TOTAL_CONNECTIONS"] = totalConnections;
                    variables["AVAILABLE_CONNECTIONS"] = availableConnections;
                    
                    std::string html = utils::TemplateLoader::loadTemplate("db_test.html", variables);
                    res.setHtml(html);
                } else {
                    // 返回JSON数据
                    res.setJson(R"({
                        "status": "success",
                        "message": "Database connection successful",
                        "test_value": )" + testValue + R"(,
                        "pool_status": {
                            "total": )" + totalConnections + R"(,
                            "available": )" + availableConnections + R"(
                        }
                    })");
                }
            } else {
                res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
                if (accept.find("text/html") != std::string::npos) {
                    res.setHtml("<h1>数据库错误</h1><p>没有返回结果</p><a href='/'>返回首页</a>");
                } else {
                    res.setJson(R"({"status": "error", "message": "No results returned"})");
                }
            }
        } catch (const std::exception& e) {
            g_dbPool->returnConnection(conn);
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            if (accept.find("text/html") != std::string::npos) {
                res.setHtml("<h1>数据库错误</h1><p>异常: " + std::string(e.what()) + "</p><a href='/'>返回首页</a>");
            } else {
                res.setJson(R"({"status": "error", "message": ")" + std::string(e.what()) + R"("})");
            }
        }
    });
    
    
    // 12. 查看所有用户
    router->get("/db/users", [](const http::HttpRequest& req, http::HttpResponse& res) {
        std::string accept = req.getHeader("Accept");
        if (!g_dbPool) {
            res.setStatus(http::HttpStatus::SERVICE_UNAVAILABLE);
            res.setJson(R"({"status": "error", "message": "Database not configured"})");
            return;
        }
        auto conn = g_dbPool->getConnection();
        if (!conn) {
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            if (accept.find("text/html") != std::string::npos) {
                res.setHtml("<h1>数据库错误</h1><p>无法获取数据库连接</p><a href='/'>返回首页</a>");
            } else {
                res.setJson(R"({"status": "error", "message": "Failed to get database connection"})");
            }
            return;
        }

        try {
            auto result = conn->executeQuery("SELECT username, password FROM user");
            g_dbPool->returnConnection(conn);

            if (!result) {
                res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
                if (accept.find("text/html") != std::string::npos) {
                    res.setHtml("<h1>数据库错误</h1><p>查询失败</p><a href='/'>返回首页</a>");
                } else {
                    res.setJson(R"({"status": "error", "message": "Query failed"})");
                }
                return;
            }

            if (accept.find("text/html") != std::string::npos) {
                // 使用模板文件
                std::string userRows = "";
                int userCount = 0;
                
                while (result->next()) {
                    userRows += "<tr>";
                    userRows += "<td>" + result->getString("username") + "</td>";
                    userRows += "<td>" + result->getString("password") + "</td>";
                    userRows += "</tr>";
                    userCount++;
                }
                
                std::map<std::string, std::string> variables;
                variables["USER_ROWS"] = userRows;
                variables["USER_COUNT"] = std::to_string(userCount);
                
                std::string html = utils::TemplateLoader::loadTemplate("db_users.html", variables);
                res.setHtml(html);
            } else {
                // 返回JSON数据
                res.setStatus(http::HttpStatus::OK);
                res.setHeader("Content-Type", "application/json");
                
                std::string json = R"({"status": "success", "users": [)";
                bool first = true;
                
                while (result->next()) {
                    if (!first) json += ",";
                    json += R"({"username": ")" + result->getString("username") + 
                           R"(", "password": ")" + result->getString("password") + R"("})";
                    first = false;
                }
                
                json += "]}";
                res.setBody(json);
            }
            
        } catch (const std::exception& e) {
            g_dbPool->returnConnection(conn);
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            if (accept.find("text/html") != std::string::npos) {
                res.setHtml("<h1>数据库错误</h1><p>异常: " + std::string(e.what()) + "</p><a href='/'>返回首页</a>");
            } else {
                res.setJson(R"({"status": "error", "message": ")" + std::string(e.what()) + R"("})");
            }
        }
    });
    
    // 12. 添加用户表单
    router->get("/db/add-user", [](const http::HttpRequest& req, http::HttpResponse& res) {
        (void)req;
        std::string html = utils::TemplateLoader::loadTemplate("add_user.html");
        res.setHtml(html);
    });
    
    
    // 13. 添加用户 (POST)
    router->post("/db/users", [](const http::HttpRequest& req, http::HttpResponse& res) {
        if (!g_dbPool) {
            res.setStatus(http::HttpStatus::SERVICE_UNAVAILABLE);
            res.setJson(R"({"status": "error", "message": "Database not configured"})");
            return;
        }
        auto conn = g_dbPool->getConnection();
        if (!conn) {
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            res.setJson(R"({"status": "error", "message": "Failed to get database connection"})");
            return;
        }

        try {
            std::string body = req.getBody();
            std::string username, password;
            
            // 调试输出
            std::cout << "POST body: " << body << std::endl;
            
            size_t usernamePos = body.find("username=");
            size_t passwordPos = body.find("password=");
            
            if (usernamePos != std::string::npos && passwordPos != std::string::npos) {
                usernamePos += 9; // "username=".length()
                passwordPos += 9; // "password=".length()
                
                size_t usernameEnd = body.find("&", usernamePos);
                size_t passwordEnd = body.find("&", passwordPos);
                
                if (usernameEnd == std::string::npos) usernameEnd = body.length();
                if (passwordEnd == std::string::npos) passwordEnd = body.length();
                
                username = body.substr(usernamePos, usernameEnd - usernamePos);
                password = body.substr(passwordPos, passwordEnd - passwordPos);
                
                // URL解码
                username = urlDecode(username);
                password = urlDecode(password);
            }

            std::cout << "Parsed username: '" << username << "', password: '" << password << "'" << std::endl;

            if (username.empty() || password.empty()) {
                g_dbPool->returnConnection(conn);
                res.setStatus(http::HttpStatus::BAD_REQUEST);
                res.setJson(R"({"status": "error", "message": "Username and password are required", "debug": {"body": ")" + body + R"(", "username": ")" + username + R"(", "password": ")" + password + R"("}})");
                return;
            }

            std::string insertSql = "INSERT INTO user (username, password) VALUES ('" + username + "', '" + password + "')";
            int affected = conn->executeUpdate(insertSql);
            g_dbPool->returnConnection(conn);

            if (affected > 0) {
                res.setJson(R"({
                    "status": "success",
                    "message": "User added successfully",
                    "username": ")" + username + R"(",
                    "affected_rows": )" + std::to_string(affected) + R"(
                })");
            } else {
                res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
                res.setJson(R"({"status": "error", "message": "Failed to add user"})");
            }
            
        } catch (const std::exception& e) {
            g_dbPool->returnConnection(conn);
            res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
            res.setJson(R"({"status": "error", "message": ")" + std::string(e.what()) + R"("})");
        }
    });
    
    
    
    return router;
}

int main() {
    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        std::cout << " 启动 HttpFramework 综合测试服务器..." << std::endl;
        
        // 1. 初始化数据库连接池（可选，失败不影响启动）
        std::cout << " 初始化数据库连接池..." << std::endl;
        g_dbPool = std::make_shared<db::DbConnectionPool>(
            "localhost", "suzune", "123456", "httpframework", 3306, 5
        );

        if (!g_dbPool->initialize()) {
            std::cerr << " 数据库连接池初始化失败（DB功能不可用），继续启动..." << std::endl;
            g_dbPool.reset();
        } else {
            std::cout << " 数据库连接池初始化成功" << std::endl;
        }
        
        // 2. 初始化会话管理器
        std::cout << " 初始化会话管理器..." << std::endl;
        g_sessionManager = std::make_shared<session::SessionManager>(std::chrono::seconds(1800)); // 30分钟过期
        g_sessionManager->startCleanupThread();
        std::cout << " 会话管理器初始化成功" << std::endl;
        
        // 3. 创建路由
        std::cout << " 配置路由..." << std::endl;
        auto router = createRouter();
        std::cout << " 路由配置完成" << std::endl;
        
        // 4. 启动HTTP服务器
        std::cout << " 启动HTTP服务器 (端口 8080)..." << std::endl;
        
        // 检查端口是否被占用
        if (isPortInUse(8080)) {
            std::cout << "  端口8080被占用，尝试使用SO_REUSEADDR..." << std::endl;
        }
        
        g_httpServer = std::make_shared<http::HttpServer>(8080, 8); // 8个工作线程
        g_httpServer->setRouter(router);
        
        // 启用内存池以支持高并发
        g_httpServer->enableMemoryPool(true);
        
        if (!g_httpServer->start()) {
            std::cerr << " HTTP服务器启动失败" << std::endl;
            std::cerr << " 提示: 请检查端口8080是否被其他进程占用" << std::endl;
            std::cerr << "   可以使用命令: netstat -tlnp | grep :8080" << std::endl;
            return 1;
        }
        std::cout << " HTTP服务器启动成功: http://localhost:8080" << std::endl;
        
        
        // 6. 显示服务器信息
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << " HttpFramework 综合测试服务器启动完成!" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << " HTTP服务器:  http://localhost:8080" << std::endl;
        std::cout << " 架构: Reactor + 线程池 (8个工作线程)" << std::endl;
        std::cout << " 特性: epoll ET模式, 非阻塞I/O, 内存池优化" << std::endl;
        std::cout << " 数据库: " << (g_dbPool ? "MySQL (httpframework)" : "未启用") << std::endl;
        std::cout << " 会话管理: 已启用 (30分钟过期)" << std::endl;
        if (g_dbPool) {
            std::cout << " 连接池: " << g_dbPool->getTotalConnections() << " 个连接" << std::endl;
        }
        std::cout << std::string(60, '=') << std::endl;
        std::cout << " 访问 http://localhost:8080 查看完整功能演示" << std::endl;
        std::cout << " 按 Ctrl+C 停止服务器" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        // 7. 主循环
        while (g_httpServer->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << " 服务器启动失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
