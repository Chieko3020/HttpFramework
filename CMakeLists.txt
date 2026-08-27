cmake_minimum_required(VERSION 3.16)
project(HttpFramework VERSION 1.0.0 LANGUAGES CXX)

# 禁止源码内构建，防止 make clean 误删源文件
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR
        "禁止在源码目录内构建 (in-source build)！\n"
        "请使用独立的 build 目录，例如:\n"
        "  cmake -S . -B build\n"
        "  cmake --build build\n"
        "如果已经污染了源码目录，请手动删除 CMakeCache.txt 和 CMakeFiles/")
endif()

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")

# 作为顶层项目时的选项
option(BUILD_EXAMPLES "Build example applications" ON)

# 标准安装路径
include(GNUInstallDirs)

# 查找依赖库
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# 查找MySQL Connector/C++
find_path(MYSQLCPPCONN_INCLUDE_DIR mysql_connection.h
    PATHS /usr/include/cppconn /usr/local/include/cppconn
    PATH_SUFFIXES cppconn)

find_library(MYSQLCPPCONN_LIBRARY mysqlcppconn
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
    PATH_SUFFIXES x86_64-linux-gnu)

if(NOT MYSQLCPPCONN_LIBRARY)
    find_library(MYSQLCPPCONN_LIBRARY mysqlcppconn7t64
        PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
endif()

if(MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONN_LIBRARY)
    set(MYSQLCPPCONN_FOUND TRUE)
    message(STATUS "Found MySQL Connector/C++: ${MYSQLCPPCONN_LIBRARY}")
    message(STATUS "MySQL Connector/C++ include dir: ${MYSQLCPPCONN_INCLUDE_DIR}")
else()
    set(MYSQLCPPCONN_FOUND FALSE)
    message(WARNING "MySQL Connector/C++ not found. Database features will be disabled.")
    message(STATUS "Please install: sudo apt install libmysqlcppconn-dev libmysqlcppconn7t64")
endif()

# 构建库
add_subdirectory(src)

# 模板文件复制 (configure-time + rebuild on change)
add_custom_target(copy_templates ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/templates
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/templates
        ${CMAKE_BINARY_DIR}/templates
    COMMENT "Copying template files to build directory"
    DEPENDS
        ${CMAKE_SOURCE_DIR}/templates/index.html
        ${CMAKE_SOURCE_DIR}/templates/api_status.html
        ${CMAKE_SOURCE_DIR}/templates/api_time.html
        ${CMAKE_SOURCE_DIR}/templates/api_stats.html
        ${CMAKE_SOURCE_DIR}/templates/db_test.html
        ${CMAKE_SOURCE_DIR}/templates/db_users.html
        ${CMAKE_SOURCE_DIR}/templates/session_stats.html
        ${CMAKE_SOURCE_DIR}/templates/session_info.html
        ${CMAKE_SOURCE_DIR}/templates/add_user.html
)

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

add_subdirectory(tests)

# ---- 安装规则 ----
install(TARGETS http_framework
    EXPORT HttpFrameworkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY templates/ DESTINATION ${CMAKE_INSTALL_DATADIR}/HttpFramework/templates)

# CMake 包配置，使外部项目可通过 find_package(HttpFramework) 引用
install(EXPORT HttpFrameworkTargets
    FILE HttpFrameworkTargets.cmake
    NAMESPACE HttpFramework::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HttpFramework
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/HttpFrameworkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HttpFrameworkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HttpFramework
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HttpFrameworkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HttpFrameworkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HttpFrameworkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HttpFramework
)
