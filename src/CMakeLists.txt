# http_framework 静态库 - 源文件列表（显式枚举，防止遗漏）
set(HTTP_SOURCES
    http/HttpResponse.cpp
    http/HttpRequest.cpp
    http/HttpContext.cpp
    http/HttpServer.cpp
    middleware/SessionMiddleware.cpp
    utils/MemoryPool.cpp
    utils/ThreadPool.cpp
    session/Session.cpp
    session/SessionStorage.cpp
    session/SessionManager.cpp
    router/RouterHandler.cpp
    router/Router.cpp
    utils/db/DbConnection.cpp
    utils/db/DbException.cpp
    utils/db/DbConnectionPool.cpp
)

add_library(http_framework STATIC ${HTTP_SOURCES})

target_include_directories(http_framework PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(http_framework
    Threads::Threads
    Boost::system
)

# 编译期注入模板路径，使可执行文件在任何工作目录下都能找到模板
target_compile_definitions(http_framework PUBLIC
    TEMPLATES_DIR="${CMAKE_BINARY_DIR}/templates/"
)

# 如果找到MySQL Connector/C++，链接它
if(MYSQLCPPCONN_FOUND)
    target_include_directories(http_framework PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR})
    target_link_libraries(http_framework ${MYSQLCPPCONN_LIBRARY})
    target_compile_definitions(http_framework PRIVATE ENABLE_DATABASE)
endif()
