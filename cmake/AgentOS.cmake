# ── AgentOS.cmake ──
# Helper macros for downstream projects that use find_package(AgentOS).
#
# Provides:
#   agentos_add_agent(name SOURCES src1.cpp ... [LINKS lib1 ...])
#
# Example:
#   find_package(AgentOS REQUIRED)
#   agentos_add_agent(my_agent SOURCES main.cpp utils.cpp LINKS my_lib)

#[=======================================================================[.rst:
agentos_add_agent
------------------

Create an executable target that links against AgentOS::AgentOS and
any additional libraries.

.. code-block:: cmake

  agentos_add_agent(<name>
      SOURCES <source1> [<source2> ...]
      [LINKS <lib1> [<lib2> ...]]
  )

``<name>``
  Name of the executable target to create.

``SOURCES``
  One or more C++ source files.

``LINKS``
  Optional additional libraries to link (beyond AgentOS::AgentOS).

The macro also sets C++23 on the target and enables
position-independent code.
#]=======================================================================]

macro(agentos_add_agent _name)
    # ── Parse arguments ──
    cmake_parse_arguments(_AOS "" "" "SOURCES;LINKS" ${ARGN})

    if(NOT _AOS_SOURCES)
        message(FATAL_ERROR "agentos_add_agent(${_name}): "
                "SOURCES must list at least one source file.")
    endif()

    add_executable(${_name} ${_AOS_SOURCES})

    target_link_libraries(${_name} PRIVATE AgentOS::AgentOS)

    # Ensure C++23 (matches upstream requirement)
    target_compile_features(${_name} PRIVATE cxx_std_23)

    # Additional link libraries requested by the caller
    if(_AOS_LINKS)
        target_link_libraries(${_name} PRIVATE ${_AOS_LINKS})
    endif()
endmacro()
