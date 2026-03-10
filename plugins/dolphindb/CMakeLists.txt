cmake_minimum_required(VERSION 3.13)
project(PluginAgentOS VERSION 1.0.0 LANGUAGES CXX)

# DolphinDB 插件要求 C++17（与 server ABI 保持一致）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── DolphinDB SDK 头文件 ──
# 用户需设置 DOLPHINDB_INCLUDE_DIR 指向 DolphinDBPlugin/include
# 例如: cmake -DDOLPHINDB_INCLUDE_DIR=/path/to/DolphinDBPlugin/include
set(DOLPHINDB_INCLUDE_DIR "" CACHE PATH "Path to DolphinDB SDK include directory (contains CoreConcept.h)")
if(NOT DOLPHINDB_INCLUDE_DIR)
    find_path(DOLPHINDB_INCLUDE_DIR
        NAMES CoreConcept.h
        HINTS
            /opt/dolphindb/server/include
            /usr/local/dolphindb/include
            $ENV{DOLPHINDB_HOME}/include
            $ENV{DOLPHINDB_PLUGIN_DIR}/../include
            ${CMAKE_SOURCE_DIR}/../../deps/dolphindb/include
    )
    if(NOT DOLPHINDB_INCLUDE_DIR)
        message(FATAL_ERROR
            "DolphinDB SDK not found. Set DOLPHINDB_INCLUDE_DIR:\n"
            "  cmake -DDOLPHINDB_INCLUDE_DIR=/path/to/DolphinDBPlugin/include ..")
    endif()
endif()
message(STATUS "DolphinDB SDK include: ${DOLPHINDB_INCLUDE_DIR}")

# ── DolphinDB 共享库（libDolphinDB.so）──
set(DOLPHINDB_LIB_DIR "" CACHE PATH "Path to libDolphinDB.so directory")
find_library(DOLPHINDB_LIBRARY
    NAMES DolphinDB
    HINTS
        ${DOLPHINDB_LIB_DIR}
        /opt/dolphindb/server
        /usr/local/dolphindb/lib
        $ENV{DOLPHINDB_HOME}/lib
        $ENV{DOLPHINDB_HOME}
)

# ── AgentOS 库 ──
# 方式一：作为父项目的子目录（TARGET agentos 已存在）
# 方式二：独立构建，手动指定路径
set(AGENTOS_ROOT_DIR "${CMAKE_SOURCE_DIR}/../.." CACHE PATH "AgentOS project root")
set(AGENTOS_BUILD_DIR "${AGENTOS_ROOT_DIR}/build" CACHE PATH "AgentOS build directory")

if(TARGET agentos)
    message(STATUS "PluginAgentOS: using AgentOS target from parent build")
else()
    find_library(AGENTOS_LIBRARY
        NAMES agentos
        HINTS ${AGENTOS_BUILD_DIR}
        NO_DEFAULT_PATH
    )
    if(NOT AGENTOS_LIBRARY)
        message(FATAL_ERROR
            "libagentos.a not found in ${AGENTOS_BUILD_DIR}.\n"
            "Build AgentOS first: cd ${AGENTOS_ROOT_DIR} && cmake -B build && cmake --build build")
    endif()
    message(STATUS "AgentOS library: ${AGENTOS_LIBRARY}")
endif()

# ── 插件动态库 ──
add_library(PluginAgentOS SHARED
    dolphindb_adapter.cpp
)

# 输出文件名: libPluginAgentOS.so（与 PluginAgentOS.txt 第1行匹配）
set_target_properties(PluginAgentOS PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "PluginAgentOS"
)

# Include paths
target_include_directories(PluginAgentOS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AGENTOS_ROOT_DIR}/include
)

# DolphinDB SDK headers as SYSTEM（抑制第三方警告）
target_include_directories(PluginAgentOS SYSTEM PRIVATE
    ${DOLPHINDB_INCLUDE_DIR}
)

# ── 必须的编译宏（DolphinDB ABI 兼容）──
target_compile_definitions(PluginAgentOS PRIVATE
    LINUX                       # 平台标识（Windows 用 WINDOWS）
    LOCKFREE_SYMBASE            # DolphinDB release 1.30+ 必须
)

# GCC ABI 兼容（DolphinDB server 使用旧 ABI）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(PluginAgentOS PRIVATE
        _GLIBCXX_USE_CXX11_ABI=0
    )
endif()

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
    )
    if(DUCKDB_LIBRARY)
        target_link_libraries(PluginAgentOS PRIVATE ${DUCKDB_LIBRARY})
    endif()
    target_include_directories(PluginAgentOS PRIVATE
        ${SQLITE3_INCLUDE_DIRS}
    )
endif()

if(DOLPHINDB_LIBRARY)
    target_link_libraries(PluginAgentOS PRIVATE ${DOLPHINDB_LIBRARY})
    message(STATUS "DolphinDB library: ${DOLPHINDB_LIBRARY}")
else()
    message(STATUS "DolphinDB library not found (will rely on runtime linking)")
endif()

# Compiler warnings
target_compile_options(PluginAgentOS PRIVATE
    -Wall -Wextra -O2
    -Wno-unused-parameter          # DolphinDB SDK 回调签名需要
)

# ── 安装规则 ──
set(DOLPHINDB_PLUGIN_DIR "" CACHE PATH "DolphinDB plugin install directory")
if(DOLPHINDB_PLUGIN_DIR)
    install(TARGETS PluginAgentOS LIBRARY DESTINATION ${DOLPHINDB_PLUGIN_DIR}/AgentOS)
    install(FILES PluginAgentOS.txt DESTINATION ${DOLPHINDB_PLUGIN_DIR}/AgentOS)
else()
    install(TARGETS PluginAgentOS LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/dolphindb/plugins/AgentOS)
    install(FILES PluginAgentOS.txt DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/dolphindb/plugins/AgentOS)
endif()

message(STATUS "PluginAgentOS ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID}")
