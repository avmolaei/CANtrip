# /FS applies to everything we build, including vendored third-party code
# (dbcppp): without it, MSBuild's parallel cl.exe invocations for multi-file
# targets randomly fail with C1041 "cannot open program database" as they
# race on the same .pdb. NOMINMAX/WIN32_LEAN_AND_MEAN are likewise harmless
# everywhere. /W4 and friends are NOT applied globally - dumping our own
# strict warning level onto vendored code (dbcppp, Boost) just floods the
# build log with warnings we can't (and shouldn't) act on.
if(MSVC)
    add_compile_options(/FS)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Call on CANtrip's own targets (not third-party ones) to opt into our
# actual warning level.
function(cantrip_target_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()
