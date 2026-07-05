# Shared library: the vendor-neutral AVlabs CAN backend interface plus every
# implementation of it (PeakBackend, VectorBackend today; more vendors slot
# in here as CanBackendRegistry.cpp grows to reference them).
add_library(cantrip_common STATIC
    AVlabsCanBackend.h
    CanBackendRegistry.cpp
    PeakBackend.h
    PeakBackend.cpp
    VectorBackend.h
    VectorBackend.cpp
)

target_include_directories(cantrip_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
cantrip_target_options(cantrip_common)
