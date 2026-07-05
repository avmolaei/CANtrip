cmake_minimum_required(VERSION 3.21)
project(CANtrip CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "CANtrip targets Windows only (its CAN backends and the Wireshark extcap plumbing are Windows-specific).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CANtripCompilerOptions)

option(CANTRIP_BUILD_APP "Build the CANtrip Qt desktop app (requires Qt6)" OFF)

# dbcppp only needs to parse .dbc files for us - skip its KCD (XML) support,
# CLI tool, and test/example targets so we don't drag in libxml2/cxxopts.
set(build_kcd OFF CACHE BOOL "" FORCE)
set(build_tools OFF CACHE BOOL "" FORCE)
set(build_tests OFF CACHE BOOL "" FORCE)
set(build_examples OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dbcppp)
# dbcppp's own src/CMakeLists.txt (which sets this) isn't actually used by
# its root CMakeLists.txt - a quirk in the upstream project, not us being
# clever - so without this every exported symbol stays __declspec(dllimport)
# even while building the DLL itself, which MSVC rightly refuses to link.
target_compile_definitions(dbcppp PRIVATE DBCPPP_EXPORT)

add_subdirectory(common)
add_subdirectory(extcap)

if(CANTRIP_BUILD_APP)
    add_subdirectory(app)
endif()
