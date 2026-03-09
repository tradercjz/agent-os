cmake_minimum_required(VERSION 3.25)
project(AgentOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler-specific flags for C++23 coroutines
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -fcoroutines -O2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -stdlib=libc++ -O2)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# ---- nlohmann/json ----
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- libcurl ----
find_package(CURL REQUIRED)

# ---- SQLite3 ----
# macOS Homebrew: brew install sqlite
# Linux: apt install libsqlite3-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# ---- DuckDB ----
# macOS: brew install duckdb
# Linux: see https://duckdb.org/docs/installation
find_library(DUCKDB_LIBRARY duckdb HINTS /opt/homebrew/lib /usr/local/lib)
find_path(DUCKDB_INCLUDE_DIR duckdb.hpp HINTS /opt/homebrew/include /usr/local/include)
if(NOT DUCKDB_LIBRARY OR NOT DUCKDB_INCLUDE_DIR)
    message(FATAL_ERROR "DuckDB not found. Install via: brew install duckdb")
endif()
message(STATUS "DuckDB: ${DUCKDB_LIBRARY}")

# ---- GoogleTest ----
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# ---- hnswlib ----
FetchContent_Declare(
  hnswlib
  URL https://github.com/nmslib/hnswlib/archive/refs/tags/v0.8.0.zip
)
FetchContent_MakeAvailable(hnswlib)

# ---- cppjieba (optional) ----
option(AGENTOS_ENABLE_JIEBA "Enable cppjieba Chinese tokenization support" ON)

if(AGENTOS_ENABLE_JIEBA)
    FetchContent_Declare(
      cppjieba
      GIT_REPOSITORY https://github.com/yanyiwu/cppjieba.git
      GIT_TAG v5.4.0
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cppjieba)

    if(NOT TARGET cppjieba::cppjieba)
        add_library(cppjieba_iface INTERFACE)
        target_include_directories(cppjieba_iface INTERFACE
            ${cppjieba_SOURCE_DIR}/include
            ${cppjieba_SOURCE_DIR}/deps
            ${cppjieba_SOURCE_DIR}/deps/limonp/include
        )
        add_library(cppjieba::cppjieba ALIAS cppjieba_iface)
    endif()
endif()

# ---- Library sources ----
set(AGENTOS_SOURCES
    src/kernel/llm_kernel.cpp
    src/scheduler/scheduler.cpp
    src/context/context.cpp
    src/memory/memory.cpp
    src/tools/tool_manager.cpp
    src/security/security.cpp
    src/bus/agent_bus.cpp
)

add_library(agentos STATIC ${AGENTOS_SOURCES})
target_include_directories(agentos PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(agentos PUBLIC ${SQLITE3_INCLUDE_DIRS})
target_include_directories(agentos PUBLIC ${DUCKDB_INCLUDE_DIR})
target_link_libraries(agentos PUBLIC Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl hnswlib::hnswlib ${SQLITE3_LIBRARIES} ${DUCKDB_LIBRARY})

if(AGENTOS_ENABLE_JIEBA)
    target_link_libraries(agentos PUBLIC cppjieba::cppjieba)
    target_compile_definitions(agentos PUBLIC
        AGENTOS_ENABLE_JIEBA=1
        CPPJIEBA_DICT_DIR="${cppjieba_SOURCE_DIR}/dict"
    )
endif()

# ---- Examples ----
add_executable(agentos_demo examples/demo.cpp)
target_link_libraries(agentos_demo agentos)
add_executable(graph_demo examples/graph_demo.cpp)
target_link_libraries(graph_demo agentos)

add_executable(temporal_graph_demo examples/temporal_graph_demo.cpp)
target_link_libraries(temporal_graph_demo agentos)

add_executable(csr_graph_demo examples/csr_graph_demo.cpp)
target_link_libraries(csr_graph_demo agentos)

add_executable(openai_demo examples/openai_demo.cpp)
target_link_libraries(openai_demo agentos)

add_executable(rag_agent_demo examples/rag_agent_demo.cpp)
target_link_libraries(rag_agent_demo agentos)

# ---- Tests ----
add_executable(benchmark_csr_graph tests/benchmark_csr_graph.cpp)
target_link_libraries(benchmark_csr_graph PRIVATE agentos)

enable_testing()
add_executable(
  test_agentos
  tests/test_graph_memory.cpp
  tests/test_llm_kernel.cpp
  tests/test_memory_system.cpp
  tests/test_tool_manager.cpp
  tests/test_scheduler.cpp
  tests/test_context.cpp
  tests/test_security.cpp
  tests/test_security_enhanced.cpp
  tests/test_bus.cpp
  tests/test_agent.cpp
  tests/test_csr_graph.cpp
  tests/test_knowledge_base.cpp
  tests/test_sqlite_store.cpp
  tests/test_duckdb_store.cpp
  tests/test_integration.cpp
  tests/test_logger.cpp
)
target_link_libraries(test_agentos PRIVATE agentos GTest::gtest_main GTest::gtest)
include(GoogleTest)
gtest_discover_tests(test_agentos)

# ---- Benchmarks ----
add_executable(benchmark_graph_memory tests/benchmark_graph_memory.cpp)
target_link_libraries(benchmark_graph_memory PRIVATE agentos)

# ---- Build info ----
message(STATUS "AgentOS ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Targets: agentos_demo (Mock), openai_demo (Real LLM via curl)")
