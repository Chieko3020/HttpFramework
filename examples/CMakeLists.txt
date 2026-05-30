# 最简示例（无数据库依赖）
add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE http_framework)

# 全功能 demo（需要 MySQL，未配置时自动降级）
add_executable(full_demo full_demo.cpp)
target_link_libraries(full_demo PRIVATE http_framework)

# 性能基准测试服务器
add_executable(bench_server bench_server.cpp)
target_link_libraries(bench_server PRIVATE http_framework)

# WSS echo 示例 + WSS 基准客户端（需要 ENABLE_WSS=ON）
if(ENABLE_WSS)
    add_executable(wss_echo wss_echo.cpp)
    target_link_libraries(wss_echo PRIVATE http_framework)

    add_executable(wss_bench_client wss_bench_client.cpp)
    target_link_libraries(wss_bench_client PRIVATE http_framework OpenSSL::SSL)
endif()

