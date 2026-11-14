find_package(Qt6 REQUIRED COMPONENTS Widgets Charts Svg PrintSupport)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Everything except main() lives in a static library, so the unit tests in
# test/unit link the exact code the app runs instead of compiling their own
# copies of it.
add_library(cantrip_app STATIC
    MainWindow.h
    MainWindow.cpp
    BusAutoDetector.h
    BusAutoDetector.cpp
    BusLoadTracker.h
    BusLoadTracker.cpp
    BusLoadView.h
    BusLoadView.cpp
    CanControllerDialog.h
    CanControllerDialog.cpp
    GraphAxisDialog.h
    GraphAxisDialog.cpp
    GraphView.h
    GraphView.cpp
    GraphWindowContainer.h
    GraphWindowContainer.cpp
    HeadlessRunner.h
    HeadlessRunner.cpp
    AscLogWriter.h
    AscLogWriter.cpp
    CsvLogWriter.h
    CsvLogWriter.cpp
    DbcDecoder.h
    DbcDecoder.cpp
    LogFilenameTemplate.h
    LogFilenameTemplate.cpp
    LoggingOptionsDialog.h
    LoggingOptionsDialog.cpp
    LogReplaySource.h
    LogReplaySource.cpp
    LogWriter.h
    MessageSender.h
    MessageSender.cpp
    RuneFile.h
    RuneFile.cpp
    SignalHistory.h
    SignalHistory.cpp
    StatusLed.h
    StatusLed.cpp
    StimulationView.h
    StimulationView.cpp
    TraceHeaderView.h
    TraceHeaderView.cpp
    TraceModel.h
    TraceModel.cpp
    TransmitMessage.h
    TransmitMessageDialog.h
    TransmitMessageDialog.cpp
    TsharkCapture.h
    TsharkCapture.cpp
    Version.h
)

target_include_directories(cantrip_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Kept out of cantrip.exe's own folder: RELEASING.md packages that folder
# as is, and a multi-megabyte .lib has no business in the release zip.
set_target_properties(cantrip_app PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
target_link_libraries(cantrip_app PUBLIC Qt6::Widgets Qt6::Charts Qt6::Svg Qt6::PrintSupport cantrip_common dbcppp)
cantrip_target_options(cantrip_app)

add_executable(cantrip WIN32
    main.cpp
    resources/cantrip.rc
    resources/cantrip.qrc
)

target_link_libraries(cantrip PRIVATE cantrip_app)
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
