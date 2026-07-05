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

add_subdirectory(common)
add_subdirectory(extcap)

if(CANTRIP_BUILD_APP)
    add_subdirectory(app)
endif()
