find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(cantrip
    main.cpp
    MainWindow.h
    MainWindow.cpp
    StatusLed.h
    StatusLed.cpp
    TsharkCapture.h
    TsharkCapture.cpp
    resources/cantrip.rc
)

target_link_libraries(cantrip PRIVATE Qt6::Widgets cantrip_common dbcppp)
cantrip_target_options(cantrip)

# dbcppp is a DLL (see its own CMakeLists.txt - always SHARED, not our
# choice), and unlike Qt's own DLLs it isn't something windeployqt below
# knows to deploy, so cantrip.exe silently fails to start (STATUS_DLL_NOT_FOUND)
# without this.
add_custom_command(TARGET cantrip POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:dbcppp>" "$<TARGET_FILE_DIR:cantrip>"
    COMMENT "Copying dbcppp.dll next to cantrip.exe")

# Copies Qt's runtime DLLs (platform plugin, Qt6Widgets.dll, etc.) next to
# cantrip.exe after every build, so the Debug/Release output folder is
# directly runnable without manually chasing Qt's bin directory.
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET cantrip POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --no-compiler-runtime "$<TARGET_FILE:cantrip>"
        COMMENT "Running windeployqt on cantrip.exe")
endif()
