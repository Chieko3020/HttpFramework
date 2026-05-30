# 测试目标 — 每个测试独立可执行，返回 0=通过, 非 0=失败

# 公共编译定义
set(TEST_DEFS
    TEMPLATES_DIR="${CMAKE_BINARY_DIR}/templates/"
)

# ── 基础设施测试 (3 项) ──

add_executable(test_infra_threadpool test_infra_threadpool.cpp)
target_link_libraries(test_infra_threadpool PRIVATE http_framework)
target_compile_definitions(test_infra_threadpool PRIVATE ${TEST_DEFS})

add_executable(test_infra_mempool test_infra_mempool.cpp)
target_link_libraries(test_infra_mempool PRIVATE http_framework)
target_compile_definitions(test_infra_mempool PRIVATE ${TEST_DEFS})

add_executable(test_infra_logger test_infra_logger.cpp)
target_link_libraries(test_infra_logger PRIVATE http_framework)
target_compile_definitions(test_infra_logger PRIVATE ${TEST_DEFS})

# ── HTTP 功能测试 (6 项) ──

add_executable(test_http_route test_http_route.cpp)
target_link_libraries(test_http_route PRIVATE http_framework)
target_compile_definitions(test_http_route PRIVATE ${TEST_DEFS})

add_executable(test_http_middleware test_http_middleware.cpp)
target_link_libraries(test_http_middleware PRIVATE http_framework)
target_compile_definitions(test_http_middleware PRIVATE ${TEST_DEFS})

add_executable(test_http_session test_http_session.cpp)
target_link_libraries(test_http_session PRIVATE http_framework)
target_compile_definitions(test_http_session PRIVATE ${TEST_DEFS})

add_executable(test_http_response test_http_response.cpp)
target_link_libraries(test_http_response PRIVATE http_framework)
target_compile_definitions(test_http_response PRIVATE ${TEST_DEFS})

add_executable(test_http_template test_http_template.cpp)
target_link_libraries(test_http_template PRIVATE http_framework)
target_compile_definitions(test_http_template PRIVATE ${TEST_DEFS})

add_executable(test_http_db test_http_db.cpp)
target_link_libraries(test_http_db PRIVATE http_framework)
target_compile_definitions(test_http_db PRIVATE ${TEST_DEFS})
# 传递 ENABLE_DATABASE 定义给 DB 测试（来源: http_framework 的 PRIVATE 定义）
if(MYSQLCPPCONN_FOUND)
    target_include_directories(test_http_db PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR})
    target_compile_definitions(test_http_db PRIVATE ENABLE_DATABASE)
endif()

# ── 边界测试 (3 项) ──

add_executable(test_edge_input test_edge_input.cpp)
target_link_libraries(test_edge_input PRIVATE http_framework)
target_compile_definitions(test_edge_input PRIVATE ${TEST_DEFS})

# E3: 证书异常测试 — 仅在 WSS 启用时编译
if(ENABLE_WSS)
    add_executable(test_edge_cert test_edge_cert.cpp)
    target_link_libraries(test_edge_cert PRIVATE http_framework)
    target_compile_definitions(test_edge_cert PRIVATE ${TEST_DEFS})
endif()

add_executable(test_edge_stress test_edge_stress.cpp)
target_link_libraries(test_edge_stress PRIVATE http_framework)
target_compile_definitions(test_edge_stress PRIVATE ${TEST_DEFS})

# ── 自定义目标：运行所有测试 ──

# 收集所有测试目标
set(ALL_TESTS
    test_infra_threadpool
    test_infra_mempool
    test_infra_logger
    test_http_route
    test_http_middleware
    test_http_session
    test_http_response
    test_http_template
    test_http_db
    test_edge_input
    test_edge_stress
)

if(ENABLE_WSS)
    list(APPEND ALL_TESTS test_edge_cert)
endif()

# 运行单个测试的脚本模板
foreach(TEST_NAME ${ALL_TESTS})
    add_custom_target(run_${TEST_NAME}
        COMMAND $<TARGET_FILE:${TEST_NAME}>
        DEPENDS ${TEST_NAME} copy_templates
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running ${TEST_NAME}..."
    )
endforeach()

# 运行所有测试
add_custom_target(run_tests
    COMMENT "=== Running all tests ==="
)

foreach(TEST_NAME ${ALL_TESTS})
    add_custom_command(TARGET run_tests POST_BUILD
        COMMAND echo "--- ${TEST_NAME} ---"
        COMMAND $<TARGET_FILE:${TEST_NAME}>
        DEPENDS ${TEST_NAME} copy_templates
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()
