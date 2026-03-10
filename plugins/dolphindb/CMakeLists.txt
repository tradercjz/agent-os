cmake_minimum_required(VERSION 3.25)
project(PluginAgentOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── DolphinDB SDK ──
# 用户需设置 DOLPHINDB_SDK_DIR 指向 DolphinDB include 目录
# 例如: cmake -DDOLPHINDB_SDK_DIR=/path/to/dolphindb/server/include
set(DOLPHINDB_SDK_DIR "" CACHE PATH "Path to DolphinDB SDK include directory")
if(NOT DOLPHINDB_SDK_DIR)
    # 尝试常见路径
    find_path(DOLPHINDB_SDK_DIR
        NAMES CoreConcept.h
        HINTS
            /opt/dolphindb/server/include
            /usr/local/dolphindb/include
            $ENV{DOLPHINDB_HOME}/include
            ${CMAKE_SOURCE_DIR}/../../deps/dolphindb/include
    )
    if(NOT DOLPHINDB_SDK_DIR)
        message(FATAL_ERROR
            "DolphinDB SDK not found. Set DOLPHINDB_SDK_DIR:\n"
            "  cmake -DDOLPHINDB_SDK_DIR=/path/to/dolphindb/server/include ..")
    endif()
endif()
message(STATUS "DolphinDB SDK: ${DOLPHINDB_SDK_DIR}")

# ── DolphinDB 共享库 ──
find_library(DOLPHINDB_LIBRARY
    NAMES DolphinDB
    HINTS
        /opt/dolphindb/server
        /usr/local/dolphindb/lib
        $ENV{DOLPHINDB_HOME}/lib
)

# ── AgentOS 库（上层构建） ──
# 本插件需要链接 libagentos.a
# 方式一：作为子目录（在 AgentOS 根 CMakeLists.txt 中 add_subdirectory）
# 方式二：手动指定路径
set(AGENTOS_ROOT_DIR "${CMAKE_SOURCE_DIR}/../.." CACHE PATH "AgentOS project root")
set(AGENTOS_BUILD_DIR "${AGENTOS_ROOT_DIR}/build" CACHE PATH "AgentOS build directory")

# 检查是否以子项目方式构建
if(TARGET agentos)
    message(STATUS "PluginAgentOS: using AgentOS target from parent build")
else()
    # 独立构建：查找预编译的 libagentos.a
    find_library(AGENTOS_LIBRARY
        NAMES agentos
        HINTS ${AGENTOS_BUILD_DIR}
        NO_DEFAULT_PATH
    )
    if(NOT AGENTOS_LIBRARY)
        message(FATAL_ERROR
            "libagentos.a not found in ${AGENTOS_BUILD_DIR}. "
            "Build AgentOS first: cd ${AGENTOS_ROOT_DIR} && cmake -B build && cmake --build build")
    endif()
    message(STATUS "AgentOS library: ${AGENTOS_LIBRARY}")
endif()

# ── 插件动态库 ──
add_library(PluginAgentOS SHARED
    dolphindb_adapter.cpp
)

# Include paths
target_include_directories(PluginAgentOS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AGENTOS_ROOT_DIR}/include
    ${DOLPHINDB_SDK_DIR}
)

# DolphinDB SDK headers as SYSTEM to suppress third-party warnings
target_include_directories(PluginAgentOS SYSTEM PRIVATE
    ${DOLPHINDB_SDK_DIR}
)

# Link libraries
if(TARGET agentos)
    target_link_libraries(PluginAgentOS PRIVATE agentos)
else()
    target_link_libraries(PluginAgentOS PRIVATE ${AGENTOS_LIBRARY})

    # 独立构建时需要手动链接 AgentOS 的依赖
    find_package(Threads REQUIRED)
    find_package(CURL REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    find_library(DUCKDB_LIBRARY duckdb HINTS /opt/homebrew/lib /usr/local/lib)

    # nlohmann_json (header-only, fetch if needed)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)

    target_link_libraries(PluginAgentOS PRIVATE
        Threads::Threads
        CURL::libcurl
        nlohmann_json::nlohmann_json
        ${SQLITE3_LIBRARIES}
        ${DUCKDB_LIBRARY}
    )
    target_include_directories(PluginAgentOS PRIVATE
        ${SQLITE3_INCLUDE_DIRS}
    )
endif()

if(DOLPHINDB_LIBRARY)
    target_link_libraries(PluginAgentOS PRIVATE ${DOLPHINDB_LIBRARY})
endif()

# Compiler warnings (match AgentOS strictness)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(PluginAgentOS PRIVATE
        -Wall -Wextra -Wpedantic -fcoroutines -O2
        -Wno-error=unused-parameter
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(PluginAgentOS PRIVATE
        -Wall -Wextra -Wpedantic -O2
        -Wno-error=unused-parameter
    )
endif()

# ── 安装规则 ──
# 将 .so 和 .txt 安装到 DolphinDB 插件目录
set(DOLPHINDB_PLUGIN_DIR "" CACHE PATH "DolphinDB plugin install directory")
if(DOLPHINDB_PLUGIN_DIR)
    install(TARGETS PluginAgentOS LIBRARY DESTINATION ${DOLPHINDB_PLUGIN_DIR}/AgentOS)
    install(FILES PluginAgentOS.txt DESTINATION ${DOLPHINDB_PLUGIN_DIR}/AgentOS)
else()
    install(TARGETS PluginAgentOS LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/dolphindb/plugins/AgentOS)
    install(FILES PluginAgentOS.txt DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/dolphindb/plugins/AgentOS)
endif()

message(STATUS "PluginAgentOS ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD}")
