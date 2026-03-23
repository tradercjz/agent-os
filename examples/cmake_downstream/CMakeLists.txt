# ── Example: downstream project using find_package(AgentOS) ──
#
# Build instructions (assuming AgentOS has been installed):
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<agentos-install-prefix>
#   cmake --build build
#
cmake_minimum_required(VERSION 3.25)
project(MyAgentApp VERSION 1.0.0 LANGUAGES CXX)

# ── Find AgentOS (Config mode) ──
# CMAKE_PREFIX_PATH or AgentOS_DIR must point to the install tree.
find_package(AgentOS 0.1 REQUIRED)

# ── Option A: use the helper macro ──
# Automatically creates an executable, links AgentOS::AgentOS, sets C++23.
include(AgentOS)  # brings in agentos_add_agent()
agentos_add_agent(my_agent
    SOURCES main.cpp
)

# ── Option B: manual target (equivalent to the macro above) ──
# add_executable(my_agent main.cpp)
# target_link_libraries(my_agent PRIVATE AgentOS::AgentOS)
# target_compile_features(my_agent PRIVATE cxx_std_23)
