# Unit tests, one executable per area, all linked against the same
# cantrip_app library the real exe is built from. Run them with:
#   ctest --test-dir build -C Release --output-on-failure
# Widgets are tested on Qt's offscreen platform, so no window ever opens.
find_package(Qt6 REQUIRED COMPONENTS Test)

set(CMAKE_AUTOMOC ON)

get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)

function(cantrip_add_test name)
    add_executable(${name} ${name}.cpp TestFrames.h)
    target_link_libraries(${name} PRIVATE cantrip_app Qt6::Test)
    target_compile_definitions(${name} PRIVATE CANTRIP_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test")
    cantrip_target_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    # The tests run from the build tree, where neither Qt's DLLs nor
    # dbcppp.dll sit next to them. Without QT_FORCE_STDERR_LOGGING, Qt Test on
    # Windows sends its report to the debugger when no console is attached,
    # and a failing test under ctest would print nothing at all.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_qt_prefix}/plugins;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_prefix}/bin;PATH=path_list_prepend:$<TARGET_FILE_DIR:dbcppp>")
endfunction()

cantrip_add_test(tst_signal_history)
cantrip_add_test(tst_graph_view)
cantrip_add_test(tst_trace_model)
cantrip_add_test(tst_dbc_decoder)
cantrip_add_test(tst_log_formats)
cantrip_add_test(tst_rune_file)
cantrip_add_test(tst_can_bit_timing)
cantrip_add_test(tst_bus_load)
