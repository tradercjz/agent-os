cmake_minimum_required(VERSION 3.25)
project(AgentOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Version from git ──
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_VERSION)
        message(STATUS "AgentOS version from git: ${GIT_VERSION}")
    endif()
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/include/agentos/core/version.hpp.in
    ${CMAKE_BINARY_DIR}/include/agentos/core/version.hpp
    @ONLY
)

# ── Compiler-specific flags for C++23 coroutines ──
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -fcoroutines -O2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -stdlib=libc++ -O2)
endif()

# ── Sanitizers (enable via cmake -DAGENTOS_ENABLE_ASAN=ON) ──
option(AGENTOS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AGENTOS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(AGENTOS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(AGENTOS_ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(AGENTOS_ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
if(AGENTOS_ENABLE_UBSAN)
    message(STATUS "UBSanitizer enabled")
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# ── Static analysis ──
option(AGENTOS_ENABLE_CLANG_TIDY "Enable clang-tidy" OFF)
if(AGENTOS_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-checks=-*,readability-*,performance-*,bugprone-*,modernize-*")
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "clang-tidy requested but not found")
    endif()
endif()

# ── Code coverage ──
option(AGENTOS_COVERAGE "Enable code coverage instrumentation" OFF)
if(AGENTOS_COVERAGE)
    message(STATUS "Code coverage enabled")
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# ---- nlohmann/json ----
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- libcurl ----
find_package(CURL REQUIRED)

# ---- SQLite3 ----
# Try vcpkg first, then fall back to pkg-config
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(SQLITE3_TARGET unofficial::sqlite3::sqlite3)
    message(STATUS "SQLite3: found via vcpkg")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    set(SQLITE3_TARGET ${SQLITE3_LIBRARIES})
    message(STATUS "SQLite3: found via pkg-config")
endif()

# ---- DuckDB (optional) ----
# Disable with -DAGENTOS_NO_DUCKDB=ON (e.g. for DolphinDB plugin builds)
option(AGENTOS_NO_DUCKDB "Disable DuckDB backend (use FileBased/SQLite only)" OFF)
if(NOT AGENTOS_NO_DUCKDB)
    find_library(DUCKDB_LIBRARY duckdb HINTS /usr/local/lib /opt/homebrew/lib /usr/lib64 /usr/lib)
    find_path(DUCKDB_INCLUDE_DIR duckdb.hpp HINTS /usr/local/include /opt/homebrew/include /usr/include)
    if(NOT DUCKDB_LIBRARY OR NOT DUCKDB_INCLUDE_DIR)
        message(FATAL_ERROR "DuckDB not found. Install via: brew install duckdb\n"
                "Or disable with: cmake -DAGENTOS_NO_DUCKDB=ON")
    endif()
    message(STATUS "DuckDB: ${DUCKDB_LIBRARY}")
else()
    message(STATUS "DuckDB: DISABLED (AGENTOS_NO_DUCKDB=ON)")
endif()

# ---- GoogleTest ----
if(NOT TARGET GTest::gtest)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ---- hnswlib ----
if(NOT TARGET hnswlib::hnswlib)
    FetchContent_Declare(
      hnswlib
      URL https://github.com/nmslib/hnswlib/archive/refs/tags/v0.8.0.zip
    )
    FetchContent_MakeAvailable(hnswlib)
    # Mark hnswlib includes as SYSTEM to suppress third-party warnings under -Werror
    get_target_property(_hnswlib_dirs hnswlib INTERFACE_INCLUDE_DIRECTORIES)
    if(_hnswlib_dirs)
        set_target_properties(hnswlib PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hnswlib_dirs}")
    endif()
endif()

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
    src/kernel/http_client.cpp
    src/kernel/llm_kernel.cpp
    src/kernel/ollama_backend.cpp
    src/kernel/anthropic_backend.cpp
    src/kernel/llamacpp_backend.cpp
    src/scheduler/scheduler.cpp
    src/context/context.cpp
    src/memory/memory.cpp
    src/memory/consolidator.cpp
    src/tools/tool_manager.cpp
    src/tools/tool_learner.cpp
    src/security/security.cpp
    src/bus/agent_bus.cpp
    src/bus/sqlite_audit_store.cpp
    src/tracing/tracer.cpp
    src/subworkers/runtime.cpp
    src/worktree/worktree_manager.cpp
    src/core/hot_config.cpp
    src/core/co_executor.cpp
    src/core/prometheus.cpp
    src/core/key_loader.cpp
    src/core/plugin.cpp
    src/kernel/vcr_cassette.cpp
)

add_library(agentos STATIC ${AGENTOS_SOURCES})
target_include_directories(agentos PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(agentos PUBLIC ${CMAKE_BINARY_DIR}/include)
target_include_directories(agentos PUBLIC ${SQLITE3_INCLUDE_DIRS})
target_link_libraries(agentos PUBLIC Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl hnswlib::hnswlib ${SQLITE3_TARGET} ${CMAKE_DL_LIBS})

if(NOT AGENTOS_NO_DUCKDB)
    target_include_directories(agentos PUBLIC ${DUCKDB_INCLUDE_DIR})
    target_link_libraries(agentos PUBLIC ${DUCKDB_LIBRARY})
    # Treat DuckDB headers as SYSTEM includes to suppress third-party warnings
    target_include_directories(agentos SYSTEM PUBLIC ${DUCKDB_INCLUDE_DIR})
else()
    target_compile_definitions(agentos PUBLIC AGENTOS_NO_DUCKDB=1)
endif()

# R6-2: Production-grade: treat warnings as errors
target_compile_options(agentos PRIVATE
    -Werror
    -Wno-error=unused-parameter   # Allow in callback signatures
    -Wno-error=unused-function    # Allow in header-only templates
    -Wno-unknown-pragmas          # Allow omp pragmas without OpenMP enabled
)

# GCC ABI compatibility for DolphinDB plugin compatibility
# We only apply _GLIBCXX_USE_CXX11_ABI=0 to the plugin target later.
# Do not apply it globally because it breaks GTest and other standard libraries
# that are built with the new ABI.

if(AGENTOS_ENABLE_JIEBA)
    target_link_libraries(agentos PUBLIC cppjieba::cppjieba)
    target_compile_definitions(agentos PUBLIC
        AGENTOS_ENABLE_JIEBA=1
        CPPJIEBA_DICT_DIR="${cppjieba_SOURCE_DIR}/dict"
    )
endif()

# ---- llama.cpp (optional) ----
option(AGENTOS_ENABLE_LLAMACPP "Enable llama.cpp local inference backend" OFF)
if(AGENTOS_ENABLE_LLAMACPP)
    message(STATUS "llama.cpp backend: ENABLED")
    target_compile_definitions(agentos PUBLIC AGENTOS_ENABLE_LLAMACPP=1)
else()
    message(STATUS "llama.cpp backend: DISABLED (use -DAGENTOS_ENABLE_LLAMACPP=ON)")
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

add_executable(worktree_demo examples/worktree_demo.cpp)
target_link_libraries(worktree_demo agentos)

add_executable(hooks_demo examples/hooks_demo.cpp)
target_link_libraries(hooks_demo agentos)

add_executable(session_demo examples/session_demo.cpp)
target_link_libraries(session_demo agentos)

add_executable(skills_demo examples/skills_demo.cpp)
target_link_libraries(skills_demo agentos)

add_executable(mcp_demo examples/mcp_demo.cpp)
target_link_libraries(mcp_demo agentos)

add_executable(headless_demo examples/headless_demo.cpp)
target_link_libraries(headless_demo agentos)

if(NOT AGENTOS_NO_DUCKDB)
    add_executable(rag_agent_demo examples/rag_agent_demo.cpp)
    target_link_libraries(rag_agent_demo agentos)
endif()

# ---- Tests ----
add_executable(benchmark_csr_graph tests/benchmark_csr_graph.cpp)
target_link_libraries(benchmark_csr_graph PRIVATE agentos)

enable_testing()
set(TEST_SOURCES
  tests/test_graph_memory.cpp
  tests/test_llm_kernel.cpp
  tests/test_memory_system.cpp
  tests/test_tool_manager.cpp
  tests/test_scheduler.cpp
  tests/test_context.cpp
  tests/test_shortterm_regression.cpp
  tests/test_security.cpp
  tests/test_security_enhanced.cpp
  tests/test_bus.cpp
  tests/test_agent.cpp
  tests/test_reflective_agent.cpp
  tests/test_csr_graph.cpp
  tests/test_graph_builder.cpp
  tests/test_immutable_graph.cpp
  tests/test_sqlite_store.cpp
  tests/test_hindsight_store.cpp
  tests/test_sdk.cpp
  tests/test_integration.cpp
  tests/test_logger.cpp
  tests/test_api_fluent.cpp
  tests/test_compile_safety.cpp
  tests/test_consolidator.cpp
  tests/test_planning.cpp
  tests/test_tracing.cpp
  tests/test_tool_learner.cpp
  tests/test_memory_coverage.cpp
  tests/test_coverage_boost.cpp
  tests/test_supervisor.cpp
  tests/test_news_parser.cpp
  tests/test_tokenizer.cpp
  tests/test_compat.cpp
  tests/test_graph_builder.cpp
  tests/test_realtime_news_processor.cpp
  tests/test_relation_extractor.cpp
  tests/test_bm25_index.cpp
  tests/test_immutable_graph.cpp
  tests/test_document.cpp
  tests/test_task.cpp
  tests/test_agentos.cpp
  tests/test_dolphindb_adapter.cpp
  tests/test_worktree_manager.cpp
  tests/test_hooks.cpp
  tests/test_session.cpp
  tests/test_skill_registry.cpp
  tests/test_worktree_integration.cpp
  tests/test_mcp.cpp
  tests/test_headless.cpp
  tests/test_log_sink.cpp
  tests/test_audit_store.cpp
  tests/test_hot_config.cpp
  tests/test_coroutine.cpp
  tests/test_hnsw_persistence.cpp
  tests/test_http_client.cpp
  tests/test_ollama_backend.cpp
  tests/test_anthropic_backend.cpp
  tests/test_llamacpp_backend.cpp
  tests/test_prometheus.cpp
  tests/test_key_loader.cpp
  tests/test_smart_consolidation.cpp
  tests/test_vcr.cpp
  tests/test_plugin.cpp
)

# GTest issue with GCC 13: need to disable some warnings for the test target
# or use correct C++ standard macros that GTest requires.
if(NOT AGENTOS_NO_DUCKDB)
  list(APPEND TEST_SOURCES tests/test_knowledge_base.cpp)
  list(APPEND TEST_SOURCES tests/test_duckdb_store.cpp)

endif()
add_executable(test_agentos ${TEST_SOURCES})
target_link_libraries(test_agentos PRIVATE agentos GTest::gtest_main GTest::gtest)
include(GoogleTest)
gtest_discover_tests(test_agentos)

# ---- Benchmarks ----
add_executable(benchmark_graph_memory tests/benchmark_graph_memory.cpp)
target_link_libraries(benchmark_graph_memory PRIVATE agentos)

add_executable(benchmark_stress tests/benchmark_stress.cpp)
target_link_libraries(benchmark_stress PRIVATE agentos)

# ---- Fuzz targets (optional, requires clang with libFuzzer) ----
option(AGENTOS_ENABLE_FUZZER "Enable fuzz test targets" OFF)
if(AGENTOS_ENABLE_FUZZER)
    add_executable(fuzz_json_parse tests/fuzz_json_parse.cpp)
    target_link_libraries(fuzz_json_parse PRIVATE agentos)
    target_compile_options(fuzz_json_parse PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_json_parse PRIVATE -fsanitize=fuzzer)

    add_executable(fuzz_memory_entry tests/fuzz_memory_entry.cpp)
    target_link_libraries(fuzz_memory_entry PRIVATE agentos)
    target_compile_options(fuzz_memory_entry PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_memory_entry PRIVATE -fsanitize=fuzzer)
endif()

# ── Install rules ──
include(GNUInstallDirs)
install(TARGETS agentos
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/agentos
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# ── DolphinDB Plugin (optional) ──
option(AGENTOS_BUILD_DOLPHINDB_PLUGIN "Build DolphinDB plugin adapter" ON)
if(AGENTOS_BUILD_DOLPHINDB_PLUGIN)
    # 插件构建要求 AGENTOS_NO_DUCKDB=ON，否则 DolphinDB dlopen 时找不到 DuckDB 符号
    if(NOT AGENTOS_NO_DUCKDB)
        message(STATUS "DolphinDB plugin: auto-enabling AGENTOS_NO_DUCKDB (DuckDB not needed for plugin)")
        # 注意：如果已经配置了 DuckDB，这里改不了。
        # 建议直接用: cmake -DAGENTOS_BUILD_DOLPHINDB_PLUGIN=ON -DAGENTOS_NO_DUCKDB=ON
    endif()

    # Check if DolphinDB SDK is available
    set(DOLPHINDB_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/dolphindb_sdk/3.00.5" CACHE PATH "DolphinDB SDK directory")
    set(DOLPHINDB_INCLUDE_DIR "${DOLPHINDB_SDK_DIR}/include" CACHE PATH "DolphinDB include directory")
    set(DOLPHINDB_LIB_DIR "${DOLPHINDB_SDK_DIR}/lib" CACHE PATH "DolphinDB lib directory")

    if(EXISTS "${DOLPHINDB_INCLUDE_DIR}/CoreConcept.h" AND EXISTS "${DOLPHINDB_LIB_DIR}/libDolphinDB.so")
        message(STATUS "DolphinDB SDK found: ${DOLPHINDB_SDK_DIR}")
        add_subdirectory(plugins/dolphindb)

        # Set plugin output directory to build/plugins
        set_target_properties(PluginAgentOS PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
        )

        # Install plugin alongside agentos library
        install(TARGETS PluginAgentOS
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/plugins
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/plugins
        )

        message(STATUS "DolphinDB plugin will be built: ${CMAKE_BINARY_DIR}/plugins/libPluginAgentOS.so")
    else()
        message(WARNING "DolphinDB SDK not found. Plugin will not be built.")
        message(STATUS "Expected DolphinDB SDK at: ${DOLPHINDB_SDK_DIR}")
        message(STATUS "To build plugin, ensure:")
        message(STATUS "  - ${DOLPHINDB_INCLUDE_DIR}/CoreConcept.h exists")
        message(STATUS "  - ${DOLPHINDB_LIB_DIR}/libDolphinDB.so exists")
    endif()
endif()

# ── Test plugin shared library (for PluginManager tests) ──
add_library(test_plugin_sample SHARED tests/test_plugin_sample.cpp)
target_link_libraries(test_plugin_sample PRIVATE agentos)
target_compile_options(test_plugin_sample PRIVATE
    -Wno-error=unused-parameter
)

# ── Python bindings (optional, requires pybind11) ──
option(AGENTOS_ENABLE_PYTHON "Enable Python bindings via pybind11" OFF)
if(AGENTOS_ENABLE_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(agentos_py bindings/python/agentos_py.cpp)
    target_link_libraries(agentos_py PRIVATE agentos)
    target_compile_definitions(agentos_py PRIVATE AGENTOS_ENABLE_PYTHON=1)
    message(STATUS "Python bindings: ENABLED")
else()
    message(STATUS "Python bindings: DISABLED (use -DAGENTOS_ENABLE_PYTHON=ON)")
endif()

# ── Build info ──
message(STATUS "AgentOS ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Targets: agentos_demo (Mock), openai_demo (Real LLM via curl)")
