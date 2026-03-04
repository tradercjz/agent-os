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
target_link_libraries(agentos PUBLIC Threads::Threads)

# ---- Demo executable ----
add_executable(agentos_demo examples/demo.cpp)
target_link_libraries(agentos_demo PRIVATE agentos)

# ---- Build info ----
message(STATUS "AgentOS ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID}")
