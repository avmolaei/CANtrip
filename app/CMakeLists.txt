find_package(Qt6 REQUIRED COMPONENTS Widgets Charts Svg PrintSupport)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(cantrip WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
    BusAutoDetector.h
    BusAutoDetector.cpp
    CanControllerDialog.h
    CanControllerDialog.cpp
    GraphAxisDialog.h
    GraphAxisDialog.cpp
    GraphView.h
    GraphView.cpp
    GraphWindowContainer.h
    GraphWindowContainer.cpp
    AscLogWriter.h
    AscLogWriter.cpp
    CsvLogWriter.h
    CsvLogWriter.cpp
    LogFilenameTemplate.h
    LogFilenameTemplate.cpp
    LoggingOptionsDialog.h
    LoggingOptionsDialog.cpp
    LogReplaySource.h
    LogReplaySource.cpp
    LogWriter.h
    RuneFile.h
    RuneFile.cpp
    SignalHistory.h
    SignalHistory.cpp
    StatusLed.h
    StatusLed.cpp
    TsharkCapture.h
    TsharkCapture.cpp
    Version.h
    resources/cantrip.rc
    resources/cantrip.qrc
)

target_link_libraries(cantrip PRIVATE Qt6::Widgets Qt6::Charts Qt6::Svg Qt6::PrintSupport cantrip_common dbcppp)
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
