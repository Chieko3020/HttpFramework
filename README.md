# HttpFramework

- Linux 下 C++ HTTP/1.1 服务框架，基于多 Reactor + 线程池架构（main Reactor accept + sub Reactor I/O + 线程池业务），实现 Web 应用快速开发与部署
- epoll ET 模式 + 非阻塞 I/O，支持高并发
- 路由系统：静态路由、动态路由（`:id`）、通配符路由、正则匹配
- 中间件系统：链式处理，支持路径过滤，易于扩展
- 会话管理：Session/Cookie 完整生命周期管理，自动过期清理，线程安全
- MySQL 连接池：连接复用、健康检查、事务支持，简化数据库操作
- 固定大小内存池：12KB 块，零动态分配，避免内存碎片，线程安全
- 开发环境：WSL Ubuntu 24.04 LTS & Visual Studio Code, CMake 3.28.3 & MySQL 8.0.42

## 快速开始

### 依赖

```bash
sudo apt install build-essential cmake libboost-all-dev libmysqlcppconn-dev
```

MySQL Connector/C++ 可选 — 未安装时数据库功能自动禁用，服务器正常启动。

### 编译 & 运行

```bash
cmake -S . -B build && cmake --build build
./build/examples/hello_world     # 最简示例（无 DB 依赖，4 个路由）
./build/examples/full_demo       # 完整演示（路由/会话/DB/模板，MySQL 不可用时自动降级）
```

访问 `http://localhost:8080`

### 作为库安装

```bash
cmake --install build --prefix /usr/local
```

之后外部项目可通过 `find_package(HttpFramework REQUIRED)` 直接引用。

## 两种使用方式

推荐使用 `http::App` 类，将 Router、HttpServer、SessionManager、DbConnectionPool 封装在一起，提供链式 API，大幅减少样板代码。

### 方式一：编写代码

#### 最简服务器

```cpp
#include "HttpFramework.h"
int main() {
    http::App app;
    app.get("/", [](auto& req, auto& res) {
        res.setHtml("<h1>Hello, HttpFramework!</h1>");
    });
    app.start(8080);
}
```

#### 完整功能服务器

```cpp
#include "HttpFramework.h"
#include "utils/TemplateLoader.h"

int main() {
    http::App app;

    app.enableLogging()
       .enableSession()
       .enableMemoryPool();

    app.get("/", [](auto& req, auto& res) {
        res.setHtml(utils::TemplateLoader::loadTemplate("index.html"));
    });

    app.get("/api/hello", [](auto& req, auto& res) {
        res.setJson(R"({"message": "Hello from HttpFramework!"})");
    });

    app.get("/users/:id", [](auto& req, auto& res) {
        res.setJson(R"({"user_id": ")" + req.getParam("id") + R"("})");
    });

    app.get("/session", [&app](auto& req, auto& res) {
        auto s = app.sessionManager()->getSession(req.getUserData("session"));
        int count = s && s->has("visits") ? std::stoi(s->get("visits")) + 1 : 1;
        if (s) s->set("visits", std::to_string(count));
        res.setJson(R"({"visits": )" + std::to_string(count) + "}");
    });

    app.notFound([](auto& req, auto& res) {
        res.setStatus(http::HttpStatus::NOT_FOUND);
        res.setJson(R"({"error": "Not Found"})");
    });

    app.start(8080, 4);
}
```

#### 自定义中间件

```cpp
// 全局中间件 — 请求日志
app.use([](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
    std::cout << req.getMethodString() << " " << req.getPath() << std::endl;
    next();
});

// 路径特定中间件 — API 认证
app.use("/api", [](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
    if (req.getHeader("authorization").empty()) {
        res.setStatus(http::HttpStatus::UNAUTHORIZED);
        res.setJson(R"({"error": "Unauthorized"})");
        return;
    }
    next();
});

// 自定义响应头
app.use([](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
    res.setHeader("X-Powered-By", "HttpFramework");
    next();
});
```

#### 数据库集成

```cpp
http::App app;

// 启用数据库（失败时自动降级，服务器正常启动）
app.enableDatabase("localhost", "root", "password", "mydb");

app.get("/db/users", [&app](auto& req, auto& res) {
    auto pool = app.dbPool();
    if (!pool) {
        res.setJson(R"({"error": "Database not available"})");
        return;
    }
    auto conn = pool->getConnection();
    if (!conn) {
        res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
        res.setJson(R"({"error": "Failed to get connection"})");
        return;
    }
    try {
        auto result = conn->executeQuery("SELECT * FROM users");
        // ... 处理结果 ...
        pool->returnConnection(conn);
    } catch (const std::exception& e) {
        pool->returnConnection(conn);
        res.setStatus(http::HttpStatus::INTERNAL_SERVER_ERROR);
        res.setJson(R"({"error": ")" + std::string(e.what()) + R"("})");
    }
});
```

#### App 配置一览

| 方法 | 默认值 | 说明 |
|------|--------|------|
| `enableLogging()` | — | 请求日志中间件 |
| `enableSession(expire, cleanup)` | 1800s, 300s | 会话管理 + 后台自动清理 |
| `enableMemoryPool()` | — | 12KB 固定块内存池 |
| `enableDatabase(host, user, pass, db, port, max)` | port=3306, max=5 | MySQL 连接池（可选） |
| `start(port, threads)` | threads=4 | 启动服务器并阻塞等待信号 |

需要完全控制时，通过 `app.router()`、`app.sessionManager()`、`app.dbPool()`、`app.stats()` 直接操作底层对象。

### 方式二：底层 API

直接使用 `HttpServer`、`Router`、`SessionManager`、`DbConnectionPool` 等底层类，自由组装。参考 `examples/full_demo.cpp`，展示了手动管理信号处理、全局资源生命周期、端口检测等用法。

## 外部项目集成

HttpFramework 安装后可作为 CMake 包被外部项目引用。

```cmake
# 外部项目的 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(MyWebApp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(HttpFramework REQUIRED)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE HttpFramework::http_framework)
```

```cpp
// main.cpp
#include "HttpFramework.h"
int main() {
    http::App app;
    app.get("/", [](auto&, auto& res) { res.setHtml("<h1>Hello</h1>"); });
    app.start(8080);
}
```

编译时指定安装前缀：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr/local
cmake --build build
```

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_EXAMPLES` | ON | 编译示例程序 |

```bash
# 仅构建库，不编译示例
cmake -S . -B build -DBUILD_EXAMPLES=OFF
cmake --build build
```

## 模块组成

```
┌─────────────────────────────────────────────────────────┐
│  应用层    │ 路由系统 │ 中间件系统 │ 会话管理 │ 数据库集成 │
├─────────────────────────────────────────────────────────┤
│  HTTP层    │ 请求解析 │ 响应构建 │ 协议处理 │ 状态管理   │
├─────────────────────────────────────────────────────────┤
│  网络层    │ 多Reactor │ epoll ET │ 非阻塞I/O │ 线程池  │
├─────────────────────────────────────────────────────────┤
│  系统层    │ Socket编程 │ 信号处理 │ 资源管理 │ 优雅关闭  │
└─────────────────────────────────────────────────────────┘
```

### 网络层
- **多 Reactor 模式**：main Reactor 仅 accept + 轮询分发连接；sub Reactor 各自独立 epoll 处理 read/write；线程池处理业务逻辑
- **epoll ET 模式**：边缘触发，减少 epoll_wait 系统调用次数
- **非阻塞 I/O**：避免线程阻塞，提高并发能力
- **连接管理**：连接创建、复用、资源回收

### HTTP 层
- **协议解析**：HTTP/1.1 请求行、头部、主体完整解析
- **响应构建**：HTTP 响应生成和格式化
- **状态管理**：HTTP 状态码和错误处理
- **内容类型**：HTML、JSON、文本、文件等多种响应类型

### 路由层
- **静态路由**：精确路径匹配
- **动态路由**：`/users/:id` 参数提取
- **通配符路由**：`*` 通配符支持
- **正则匹配**：基于正则表达式的复杂路径匹配
- **处理器分发**：请求到处理器的映射和分发

### 中间件层
- **链式处理**：多个中间件串联执行，`next()` 控制流转
- **路径过滤**：中间件可指定作用路径前缀（如 `/api`）
- **上下文传递**：请求上下文在中间件间传递
- **内置中间件**：日志、会话管理开箱即用

### 会话层
- **会话生命周期**：创建、存储、获取、销毁
- **Cookie 处理**：会话 Cookie 自动管理
- **过期清理**：后台线程定期清理过期会话，可配置间隔
- **线程安全**：互斥锁保护并发访问
- **可扩展存储**：支持内存存储和数据库持久化

### 数据库层
- **连接池**：MySQL 连接池管理和优化
- **健康检查**：连接池健康监控和自动恢复
- **事务支持**：数据库事务的完整支持
- **异常处理**：数据库异常的捕获和处理

### 工具层
- **内存池**：12KB 固定大小内存块，O(1) 分配/释放
- **线程池**：工作线程创建和任务分发
- **模板引擎**：HTML 模板加载和 `{{variable}}` 变量替换
- **信号处理**：SIGINT/SIGTERM 优雅关闭

## 关键组件

### HttpServer — 核心服务器

多 Reactor 架构：main Reactor 线程仅处理 accept，round-robin 将连接分发给 sub Reactor；每个 sub Reactor 独立 epoll 线程处理 read/write；业务逻辑提交到线程池。

```cpp
class HttpServer {
    // main Reactor — accept + 分发
    void mainReactorLoop();
    void handleAccept();
    // sub Reactor — read/write（每个线程一个）
    void subReactorLoop(int index);
    void handleRead(int fd, int reactorIdx);
    void handleWrite(int fd, int reactorIdx);
    // 业务处理（线程池）
    void enableMemoryPool(bool enable = true);  // 启用内存池
};
```

### Router — 路由引擎

基于正则表达式的路径匹配，支持静态路由、动态参数路由、通配符路由。

```cpp
class Router {
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);
    void del(const std::string& path, Handler handler);
    void use(const std::string& path, Middleware middleware);
    void use(Middleware middleware);             // 全局中间件
    void setNotFoundHandler(Handler handler);
    Route* findRoute(const std::string& method, const std::string& path);
};
```

### SessionManager — 会话管理器

管理 Session 的完整生命周期，支持自动过期清理和线程安全访问。

```cpp
class SessionManager {
    std::shared_ptr<Session> createSession();
    std::shared_ptr<Session> getSession(const std::string& sessionId);
    void removeSession(const std::string& sessionId);
    void cleanupExpiredSessions();
    void startCleanupThread(std::chrono::seconds interval);
};
```

### MemoryPool — 内存池

固定大小内存块（12KB），预分配 5000 个块（共 60MB）。使用栈结构管理空闲块，O(1) 分配/释放，互斥锁保证线程安全。

```cpp
class MemoryPool {
    void* allocate();                           // O(1) 获取空闲块
    void deallocate(void* ptr);                 // O(1) 归还块
    size_t getAvailableBlocks() const;
    size_t getTotalBlocks() const;
};
```

## 工作原理

### 请求处理流程

```
客户端连接 → main Reactor accept → round-robin 分发给 sub Reactor
     ↓
sub Reactor epoll 监听 → 非阻塞读取 → HTTP 解析 → 提交线程池
     ↓
业务处理 → 路由匹配 → 中间件链 → 响应构建 → 数据发送 → 资源清理
```

### Reactor 事件循环

**主 Reactor** — 仅 accept + round-robin 分发连接给子 Reactor：

```cpp
void HttpServer::mainReactorLoop() {
    while (running_) {
        int n = epoll_wait(mainEpollFd_, events_, MAX_EVENTS, 1000);
        for (int i = 0; i < n; ++i) {
            if (events_[i].data.fd == listenFd_ && (events_[i].events & EPOLLIN))
                handleAccept();                 // accept → 轮询分发到 sub reactor
        }
    }
}
```

**子 Reactor** — 多个线程各带独立 epoll，处理分配的连接的 I/O：

```cpp
void HttpServer::subReactorLoop(int index) {
    while (running_) {
        int n = epoll_wait(subReactors_[index].epollFd, events, MAX_EVENTS, 1000);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) closeConnection(fd, index);
            else {
                if (events[i].events & EPOLLIN)  handleRead(fd, index);
                if (events[i].events & EPOLLOUT) handleWrite(fd, index);
            }
        }
    }
}
```

### 工作线程池

子 Reactor 读取到完整 HTTP 请求后，提交到线程池异步处理，子 Reactor 线程立即返回继续监听 I/O：

```cpp
void HttpServer::handleRead(int clientFd, int subReactorIndex) {
    std::string data = readAllData(clientFd);
    // 解析 HTTP 请求...
    auto task = std::make_shared<HttpRequestTask>(
        clientFd, request, response,
        [this, subReactorIndex](int fd, auto req, auto res) {
            processHttpRequest(subReactorIndex, fd, req, res);
        }
    );
    threadPool_->enqueue([task]() { task->execute(); });
}
```

### 边缘触发（ET）读处理

ET 模式下必须循环读取直到 EAGAIN，否则可能丢失数据：

```cpp
void handleRead(int fd) {
    char buffer[8192];
    while (true) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            // 追加到请求缓冲区
        } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
            closeConnection(fd);                // 连接关闭或错误
            break;
        } else {
            break;                              // EAGAIN — 数据读完
        }
    }
}
```

### 内存池分配

```cpp
void* MemoryPool::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (availableBlocks_.empty()) return nullptr;  // 池耗尽
    void* block = availableBlocks_.top();
    availableBlocks_.pop();
    return block;
}
```

### 会话创建与过期清理

```cpp
std::shared_ptr<Session> SessionManager::createSession() {
    std::string id = generateUniqueId();
    auto session = std::make_shared<Session>(id);
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_[id] = session;
    return session;
}

void SessionManager::cleanupExpiredSessions() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second->isExpired()) it = sessions_.erase(it);
        else ++it;
    }
}
```

## 项目难点

### 技术难点

#### 1. 高并发设计

**如何设计高效的并发模型以支持数千并发连接？**

采用多 Reactor 模型：main Reactor 线程仅 accept + round-robin 分发连接。sub Reactor 线程（默认 `hardware_concurrency` 个）各带独立 epoll，分别处理自己那组连接的 read/write，I/O 负载天然分散。业务逻辑提交到线程池异步执行，避免阻塞 sub Reactor。ET 模式减少 epoll_wait 调用频率，非阻塞 I/O 避免线程空等。

```cpp
// epoll ET 模式配置
int epollFd_ = epoll_create1(EPOLL_CLOEXEC);
struct epoll_event event;
event.events = EPOLLIN | EPOLLET;               // 边缘触发
event.data.fd = clientFd;
epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &event);
```

#### 2. 内存管理

**如何避免内存泄漏和频繁的内存分配/释放？**

预分配 12KB 固定大小内存块（5000 块，共 60MB），栈结构管理空闲块，O(1) 分配/释放。RAII + 智能指针管理对象生命周期，零拷贝减少不必要的数据复制。

```cpp
class MemoryPool {
    std::stack<void*> availableBlocks_;          // 空闲块栈，O(1) 操作
    std::mutex mutex_;                           // 线程安全
    static constexpr size_t BLOCK_SIZE = 12288;  // 12KB
};
```

#### 3. 线程安全

**多线程环境下的数据同步和竞态条件处理？**

关键数据结构使用互斥锁保护，原子变量（`std::atomic`）减少锁竞争，细粒度锁设计避免大范围加锁。SessionManager、MemoryPool、DbConnectionPool 各自持有独立锁。

```cpp
class SessionManager {
    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};
```

#### 4. 性能优化

**如何减少系统调用和内存拷贝，提高整体性能？**

- epoll ET 模式减少 epoll_wait 调用次数
- 内存池消除频繁 malloc/free 的开销和碎片
- ET 模式循环读取，一次事件通知处理全部数据
- 正则表达式编译缓存，避免重复编译路径模式

#### 5. 错误处理与资源管理

**如何设计完善的异常处理和恢复机制？**

RAII + 智能指针保证异常安全——无论正常还是异常路径，资源自动释放。数据库连接池初始化失败时自动降级，不影响服务器启动。析构函数中捕获异常，防止资源泄漏。

```cpp
class HttpServer {
    ~HttpServer() {
        try { stop(); }                         // 优雅关闭
        catch (...) { /* 不抛出 */ }
    }
};
```

### 工程难点

#### 模块解耦

通过清晰接口抽象隔离模块：Router 只关心路径匹配和处理器分发，不感知 HTTP 解析细节；Middleware 通过 `next()` 函数控制链式流转，不依赖具体业务逻辑；Session 和 DB 通过 `enable*()` 按需启用，不强制绑定。

#### API 设计

底层 API 更加灵活，App 类更加简洁，满足不同场景需求。链式调用减少中间变量，默认参数覆盖常见配置，同时保留 `router()` / `sessionManager()` / `dbPool()` 访问器直接操作底层对象。

## 项目结构

```
HttpFramework/
├── CMakeLists.txt                      # 主构建配置（含 in-source build 拦截）
├── cmake/
│   └── HttpFrameworkConfig.cmake.in    # find_package 包配置模板
├── include/
│   ├── HttpFramework.h                 # App 类
│   ├── http/                           # HttpServer / HttpRequest / HttpResponse / HttpContext
│   ├── router/                         # Router / RouterHandler
│   ├── session/                        # Session / SessionManager / SessionStorage
│   ├── middleware/                     # SessionMiddleware
│   └── utils/                          # MemoryPool / ThreadPool / TemplateLoader / db
├── src/                                # 源文件（与 include 一一对应）
├── templates/                          # HTML 模板（{{variable}} 变量替换）
├── examples/
│   ├── hello_world.cpp                 # 最简示例
│   └── full_demo.cpp                   # 全功能演示（路由/会话/DB/模板/统计）
└── init.sql                            # 数据库初始化脚本
```

## 功能验证

启动 `full_demo` 后可用端点：

| 端点 | 说明 |
|------|------|
| `http://localhost:8080/` | 首页（模板渲染） |
| `http://localhost:8080/api/status` | 服务器状态 |
| `http://localhost:8080/api/stats` | 性能统计 |
| `http://localhost:8080/api/time` | 服务端时间 |
| `http://localhost:8080/api/echo` | Echo（POST） |
| `http://localhost:8080/session/info` | 会话信息 |
| `http://localhost:8080/session/stats` | 会话统计 |
| `http://localhost:8080/db/test` | 数据库连接测试 |
| `http://localhost:8080/db/users` | 用户列表 |
| `http://localhost:8080/db/add-user` | 添加用户表单 |

```bash
# 压力测试
ab -n 10000 -c 100 http://localhost:8080/

# API 测试
curl http://localhost:8080/api/status
```

## API 参考

### App

```cpp
namespace http {
class App {
public:
    using Handler    = router::Router::Handler;
    using Middleware = router::Router::Middleware;

    // 路由注册（链式调用）
    App& get(const std::string& path, Handler h);
    App& post(const std::string& path, Handler h);
    App& put(const std::string& path, Handler h);
    App& del(const std::string& path, Handler h);
    App& patch(const std::string& path, Handler h);
    App& head(const std::string& path, Handler h);
    App& options(const std::string& path, Handler h);

    // 中间件
    App& use(Middleware m);                        // 全局
    App& use(const std::string& path, Middleware m); // 路径特定
    App& notFound(Handler h);

    // 快捷功能
    App& enableLogging();
    App& enableSession(int expirationSeconds = 1800, int cleanupIntervalSeconds = 300);
    App& enableMemoryPool(bool enable = true);
    App& enableDatabase(const std::string& host, const std::string& user,
                        const std::string& password, const std::string& database,
                        int port = 3306, int maxConnections = 5);

    // 生命周期
    void start(int port, int threads = 4);
    void stop();

    // 访问底层对象
    std::shared_ptr<router::Router>           router();
    std::shared_ptr<session::SessionManager>  sessionManager();
    std::shared_ptr<db::DbConnectionPool>     dbPool();
    const http::HttpServer::Statistics&       stats();
};
}
```

### HttpServer

```cpp
class HttpServer {
public:
    HttpServer(int port, size_t threadPoolSize = hardware_concurrency,
               size_t subReactorCount = 0);     // 0 = hardware_concurrency
    void start();
    void stop();
    void setRouter(std::shared_ptr<router::Router> router);
    void enableMemoryPool(bool enable = true);
    bool isRunning() const;
};
```

### Router

```cpp
class Router {
public:
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);
    void del(const std::string& path, Handler handler);
    void patch(const std::string& path, Handler handler);
    void head(const std::string& path, Handler handler);
    void options(const std::string& path, Handler handler);
    void use(const std::string& path, Middleware middleware);
    void use(Middleware middleware);
    void setNotFoundHandler(Handler handler);
};
```

### HttpRequest

```cpp
class HttpRequest {
public:
    HttpMethod getMethod() const;
    std::string getMethodString() const;
    std::string getPath() const;
    std::string getHeader(const std::string& name) const;
    std::string getParam(const std::string& name) const;   // 路由参数 :id
    std::string getQuery(const std::string& name) const;   // 查询参数 ?key=val
    std::string getBody() const;
    bool isKeepAlive() const;
    size_t getContentLength() const;
    void setUserData(const std::string& key, const std::string& value);
    std::string getUserData(const std::string& key) const;
};
```

### HttpResponse

```cpp
class HttpResponse {
public:
    void setStatus(HttpStatus status);
    void setHeader(const std::string& name, const std::string& value);
    void setHtml(const std::string& html);
    void setJson(const std::string& json);
    void setText(const std::string& text);
    void setFile(const std::string& filePath);
    void setBody(const std::string& body);
    void setContentType(const std::string& contentType);
};
```

## 故障排除

**编译错误**：确保依赖完整 → `apt install libboost-all-dev libmysqlcppconn-dev`，清理重编 → `rm -rf build && cmake -S . -B build && cmake --build build`

**数据库连接失败**：MySQL 不可用时 `enableDatabase()` 自动降级，检查 `sudo systemctl status mysql`，或直接不调用 `enableDatabase()`

**端口被占用**：`netstat -tlnp | grep :8080` 查看占用进程

**禁止源码内构建**：项目强制 out-of-source build。`cmake .` 会直接 FATAL_ERROR，防止 `make clean` 误删源文件。始终使用 `cmake -S . -B build`
