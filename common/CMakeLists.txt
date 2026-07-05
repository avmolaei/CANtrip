# Shared library: the vendor-neutral CanBackend interface plus every
# ICanBackend implementation (PeakBackend today; more vendors slot in here
# as CanBackendRegistry.cpp grows to reference them).
add_library(cantrip_common STATIC
    AVlabsCanBackend.h
    CanBackendRegistry.cpp
    PeakBackend.h
    PeakBackend.cpp
)

target_include_directories(cantrip_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
