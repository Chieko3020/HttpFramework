# 最简示例（无数据库依赖）
add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE http_framework)

# 全功能 demo（需要 MySQL，未配置时自动降级）
add_executable(full_demo full_demo.cpp)
target_link_libraries(full_demo PRIVATE http_framework)
