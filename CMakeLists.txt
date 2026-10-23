cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
    message(FATAL_ERROR "SerialCtl is a native Windows application")
endif()

set(SERIALCTL_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${SERIALCTL_VERSION_FILE}" SERIALCTL_VERSION_LINES ENCODING UTF-8)

list(LENGTH SERIALCTL_VERSION_LINES SERIALCTL_VERSION_LINE_COUNT)
if(NOT SERIALCTL_VERSION_LINE_COUNT EQUAL 2)
    message(FATAL_ERROR "VERSION must contain PROJECT_VERSION and CPP_VERSION")
endif()

function(read_serialctl_version VERSION_KEY OUTPUT_VARIABLE)
    set(MATCHED_VERSIONS "")
    foreach(VERSION_LINE IN LISTS SERIALCTL_VERSION_LINES)
        if(VERSION_LINE MATCHES "^${VERSION_KEY}=(V[0-9]+\\.[0-9]+\\.[0-9]+)$")
            list(APPEND MATCHED_VERSIONS "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(LENGTH MATCHED_VERSIONS MATCHED_VERSION_COUNT)
    if(NOT MATCHED_VERSION_COUNT EQUAL 1)
        message(FATAL_ERROR "VERSION must contain exactly one valid ${VERSION_KEY} record")
    endif()
    list(GET MATCHED_VERSIONS 0 MATCHED_VERSION)
    set("${OUTPUT_VARIABLE}" "${MATCHED_VERSION}" PARENT_SCOPE)
endfunction()

read_serialctl_version(PROJECT_VERSION SERIALCTL_PROJECT_VERSION)
read_serialctl_version(CPP_VERSION SERIALCTL_CPP_VERSION)
if(NOT SERIALCTL_PROJECT_VERSION STREQUAL SERIALCTL_CPP_VERSION)
    message(FATAL_ERROR "SerialCtl PROJECT_VERSION and CPP_VERSION must match")
endif()

string(REGEX REPLACE "^V" "" SERIALCTL_SEMVER "${SERIALCTL_CPP_VERSION}")
project(SerialCtl VERSION "${SERIALCTL_SEMVER}" LANGUAGES CXX RC)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SERIALCTL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(SERIALCTL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SERIALCTL_GENERATED_DIR}")

file(TO_CMAKE_PATH "${SERIALCTL_GENERATED_DIR}/app.manifest" SERIALCTL_MANIFEST_PATH)
file(TO_CMAKE_PATH "${SERIALCTL_SOURCE_DIR}/app/assets/serialctl.ico" SERIALCTL_ICON_PATH)
file(TO_CMAKE_PATH "${SERIALCTL_SOURCE_DIR}/app/assets/serialctl-light.ico" SERIALCTL_LIGHT_ICON_PATH)
file(TO_CMAKE_PATH "${SERIALCTL_SOURCE_DIR}/app/assets/toolbar-black.png" SERIALCTL_TOOLBAR_BLACK_PATH)
file(TO_CMAKE_PATH "${SERIALCTL_SOURCE_DIR}/app/assets/toolbar-white.png" SERIALCTL_TOOLBAR_WHITE_PATH)
foreach(asset surface-fill surface-stroke utility-icons)
    string(REPLACE "-" "_" variable "${asset}")
    string(TOUPPER "${variable}" variable)
    file(TO_CMAKE_PATH "${SERIALCTL_SOURCE_DIR}/app/assets/${asset}.png" SERIALCTL_${variable}_PATH)
endforeach()
configure_file("${SERIALCTL_SOURCE_DIR}/app/ui-assets.rc.in" "${SERIALCTL_GENERATED_DIR}/ui-assets.rc" @ONLY)

set(SERIALCTL_FILE_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(SERIALCTL_FILE_VERSION_DOTTED
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")

configure_file(
    "${SERIALCTL_SOURCE_DIR}/app/app.manifest.in"
    "${SERIALCTL_GENERATED_DIR}/app.manifest"
    @ONLY
    NEWLINE_STYLE UNIX
)
configure_file(
    "${SERIALCTL_SOURCE_DIR}/app/serialctl.rc.in"
    "${SERIALCTL_GENERATED_DIR}/serialctl.rc"
    @ONLY
    NEWLINE_STYLE CRLF
)

set(YY_THUNKS_PACKAGE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yy-thunks/yy-thunks.1.2.1.nupkg")
if(NOT EXISTS "${YY_THUNKS_PACKAGE}")
    message(FATAL_ERROR "YY-Thunks package is missing: ${YY_THUNKS_PACKAGE}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SerialCtl release supports x64 only")
endif()
set(SERIALCTL_ARCH "x64")
set(PUTTY_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/putty/prebuilt/x64")

set(YY_THUNKS_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/yy-thunks")
set(YY_THUNKS_OBJECT
    "${YY_THUNKS_EXTRACT_DIR}/build/native/objs/${SERIALCTL_ARCH}/YY_Thunks_for_Win7.obj")
if(NOT EXISTS "${YY_THUNKS_OBJECT}")
    file(MAKE_DIRECTORY "${YY_THUNKS_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${YY_THUNKS_PACKAGE}" DESTINATION "${YY_THUNKS_EXTRACT_DIR}")
endif()
if(NOT EXISTS "${YY_THUNKS_OBJECT}")
    message(FATAL_ERROR "YY-Thunks Win7 object is missing from ${YY_THUNKS_PACKAGE}")
endif()
set_source_files_properties("${YY_THUNKS_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE)

set(SERIALCTL_PRIVATE_INCLUDE_DIRS
    "${SERIALCTL_SOURCE_DIR}/app"
    "${SERIALCTL_SOURCE_DIR}/ai"
    "${SERIALCTL_SOURCE_DIR}/connections"
    "${SERIALCTL_SOURCE_DIR}/logging"
    "${SERIALCTL_SOURCE_DIR}/platform"
    "${SERIALCTL_SOURCE_DIR}/sftp"
    "${SERIALCTL_SOURCE_DIR}/terminal"
    "${SERIALCTL_SOURCE_DIR}/power"
    "${SERIALCTL_SOURCE_DIR}/network"
    "${SERIALCTL_SOURCE_DIR}/update"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json"
)

function(serialctl_configure_native_target TARGET_NAME SUBSYSTEM_KIND)
    target_include_directories("${TARGET_NAME}" PRIVATE ${SERIALCTL_PRIVATE_INCLUDE_DIRS})
    target_compile_features("${TARGET_NAME}" PRIVATE cxx_std_17)
    set_target_properties("${TARGET_NAME}" PROPERTIES CXX_EXTENSIONS NO)
    target_compile_definitions("${TARGET_NAME}" PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0601
        _WIN32_WINNT=0x0601
        NTDDI_VERSION=0x06010000
    )
    target_sources("${TARGET_NAME}" PRIVATE "${YY_THUNKS_OBJECT}")
    if(MSVC)
        set_property(TARGET "${TARGET_NAME}" PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options("${TARGET_NAME}" PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_link_options("${TARGET_NAME}" PRIVATE "/SUBSYSTEM:${SUBSYSTEM_KIND},6.01")
    endif()
endfunction()

add_executable(serialctl WIN32
    src/app/App.cpp
    src/app/MainWindow.cpp
    src/app/ToolbarIcons.cpp
    src/app/PowerPane.cpp
    src/app/AiPane.cpp
    src/ai/CodexClient.cpp
    src/connections/PasswordPipe.cpp
    src/update/UpdateArchive.cpp
    src/update/UpdateInflate.cpp
    src/update/UpdateClient.cpp
    src/update/UpdateManifest.cpp
    src/update/UpdateInstaller.cpp
    src/power/PowerService.cpp
    src/network/ApiServer.cpp
    src/network/WebSocket.cpp
    src/network/GatewayClient.cpp
    src/network/SessionService.cpp
    src/connections/ChildProcess.cpp
    src/connections/CmdConnection.cpp
    src/connections/CmdConsoleBridge.cpp
    src/connections/QueuedConnection.cpp
    src/connections/PlinkConnection.cpp
    src/connections/PuttyHostKey.cpp
    src/connections/SerialConnection.cpp
    src/connections/SerialDevice.cpp
    src/connections/SharedSerialConnection.cpp
    src/connections/UdpSerialConnection.cpp
    src/connections/SerialPorts.cpp
    src/connections/TcpConnection.cpp
    src/logging/SessionLogger.cpp
    src/sftp/SftpClient.cpp
    src/sftp/SftpModel.cpp
    src/terminal/TerminalModel.cpp
    "${SERIALCTL_GENERATED_DIR}/serialctl.rc"
)
serialctl_configure_native_target(serialctl WINDOWS)
target_compile_definitions(serialctl PRIVATE SERIALCTL_VERSION=L"${SERIALCTL_CPP_VERSION}")
target_compile_definitions(serialctl PRIVATE SERIALCTL_APP_VERSION="${SERIALCTL_CPP_VERSION}")
if(MSVC)
    # serialctl.rc embeds the generated, version-controlled application manifest.
    # Disable the linker-generated default manifest to avoid duplicate RT_MANIFEST
    # resources and to ensure the Win7 compatibility declarations stay authoritative.
    target_link_options(serialctl PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(serialctl PRIVATE ws2_32 setupapi msimg32 comctl32 comdlg32 shell32 ole32 advapi32 gdiplus winhttp)

foreach(PUTTY_PROGRAM IN ITEMS plink.exe psftp.exe)
    if(NOT EXISTS "${PUTTY_ARCH_DIR}/${PUTTY_PROGRAM}")
        message(FATAL_ERROR "Bundled PuTTY program is missing: ${PUTTY_ARCH_DIR}/${PUTTY_PROGRAM}")
    endif()
endforeach()

add_custom_command(TARGET serialctl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PUTTY_ARCH_DIR}/plink.exe" "$<TARGET_FILE_DIR:serialctl>/plink.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PUTTY_ARCH_DIR}/psftp.exe" "$<TARGET_FILE_DIR:serialctl>/psftp.exe"
)

install(TARGETS serialctl RUNTIME DESTINATION . COMPONENT Runtime)
install(FILES
    "${PUTTY_ARCH_DIR}/plink.exe"
    "${PUTTY_ARCH_DIR}/psftp.exe"
    DESTINATION .
    COMPONENT Runtime
)

include(CTest)
if(BUILD_TESTING)
    add_executable(ai_protocol_test tests/unit/AiProtocolTest.cpp)
    serialctl_configure_native_target(ai_protocol_test CONSOLE)
    add_test(NAME ai_protocol COMMAND ai_protocol_test)
    set_tests_properties(ai_protocol PROPERTIES TIMEOUT 30)
    add_executable(ai_fake_plink tests/integration/AiFakePlink.cpp)
    serialctl_configure_native_target(ai_fake_plink CONSOLE)
    set_target_properties(ai_fake_plink PROPERTIES OUTPUT_NAME plink RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ai-fixture")
    add_executable(ai_client_test tests/integration/AiClientTest.cpp src/ai/CodexClient.cpp
        src/connections/PasswordPipe.cpp src/connections/PuttyHostKey.cpp src/connections/ChildProcess.cpp)
    serialctl_configure_native_target(ai_client_test CONSOLE)
    target_link_libraries(ai_client_test PRIVATE advapi32 ole32)
    set_target_properties(ai_client_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ai-fixture")
    add_dependencies(ai_client_test ai_fake_plink)
    add_test(NAME ai_client COMMAND ai_client_test)
    set_tests_properties(ai_client PROPERTIES TIMEOUT 45)
    add_executable(ai_pane_test tests/integration/AiPaneTest.cpp src/app/AiPane.cpp src/app/ToolbarIcons.cpp src/ai/CodexClient.cpp
        src/connections/PasswordPipe.cpp src/connections/PuttyHostKey.cpp src/connections/ChildProcess.cpp)
    serialctl_configure_native_target(ai_pane_test CONSOLE)
    target_sources(ai_pane_test PRIVATE "${SERIALCTL_GENERATED_DIR}/ui-assets.rc")
    target_link_libraries(ai_pane_test PRIVATE ole32 msimg32)
    target_compile_definitions(ai_pane_test PRIVATE SERIALCTL_AI_TESTING)
    target_link_libraries(ai_pane_test PRIVATE advapi32 ole32 msimg32 comctl32 gdiplus gdi32 user32)
    set_target_properties(ai_pane_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ai-fixture")
    add_dependencies(ai_pane_test ai_fake_plink)
    add_test(NAME ai_pane COMMAND ai_pane_test)
    set_tests_properties(ai_pane PROPERTIES TIMEOUT 45)
    # Exercise the actual desktop device callback, not a second tool implementation.
    get_target_property(SERIALCTL_DEVICE_TEST_SOURCES serialctl SOURCES)
    list(REMOVE_ITEM SERIALCTL_DEVICE_TEST_SOURCES src/app/App.cpp "${YY_THUNKS_OBJECT}")
    add_executable(ai_device_test tests/integration/AiDeviceTest.cpp ${SERIALCTL_DEVICE_TEST_SOURCES})
    serialctl_configure_native_target(ai_device_test CONSOLE)
    target_compile_definitions(ai_device_test PRIVATE SERIALCTL_AI_TESTING
        SERIALCTL_VERSION=L"${SERIALCTL_CPP_VERSION}" SERIALCTL_APP_VERSION="${SERIALCTL_CPP_VERSION}")
    target_link_libraries(ai_device_test PRIVATE ws2_32 setupapi msimg32 comctl32 comdlg32 shell32 ole32 advapi32 gdiplus winhttp)
    if(MSVC)
        target_link_options(ai_device_test PRIVATE /MANIFEST:NO)
    endif()
    add_test(NAME ai_device COMMAND ai_device_test)
    set_tests_properties(ai_device PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(ui_buffered_paint_test tests/unit/UiBufferedPaintTest.cpp)
    serialctl_configure_native_target(ui_buffered_paint_test CONSOLE)
    target_sources(ui_buffered_paint_test PRIVATE "${SERIALCTL_GENERATED_DIR}/ui-assets.rc")
    target_link_libraries(ui_buffered_paint_test PRIVATE ole32 msimg32)
    target_link_libraries(ui_buffered_paint_test PRIVATE gdiplus gdi32 user32 comctl32)
    add_test(NAME ui_buffered_paint COMMAND ui_buffered_paint_test)

    add_executable(ui_dialog_viewport_test tests/integration/UiDialogViewportTest.cpp)
    serialctl_configure_native_target(ui_dialog_viewport_test CONSOLE)
    target_link_libraries(ui_dialog_viewport_test PRIVATE ole32 msimg32 gdiplus gdi32 user32 comctl32)
    add_test(NAME ui_dialog_viewport COMMAND ui_dialog_viewport_test)
    set_tests_properties(ui_dialog_viewport PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
    set_tests_properties(ui_buffered_paint PROPERTIES TIMEOUT 30)
    add_executable(ui_combo_paint_test tests/integration/UiComboPaintTest.cpp)
    serialctl_configure_native_target(ui_combo_paint_test CONSOLE)
    target_sources(ui_combo_paint_test PRIVATE "${SERIALCTL_GENERATED_DIR}/ui-assets.rc")
    target_link_libraries(ui_combo_paint_test PRIVATE ole32 msimg32)
    target_link_libraries(ui_combo_paint_test PRIVATE gdiplus gdi32 user32 comctl32)
    add_test(NAME ui_combo_paint COMMAND ui_combo_paint_test)
    set_tests_properties(ui_combo_paint PROPERTIES TIMEOUT 60)
    add_executable(update_client_test tests/integration/UpdateClientTest.cpp
        src/update/UpdateClient.cpp src/update/UpdateManifest.cpp src/update/UpdateArchive.cpp src/update/UpdateInflate.cpp)
    serialctl_configure_native_target(update_client_test CONSOLE)
    target_compile_definitions(update_client_test PRIVATE SERIALCTL_UPDATE_TESTING)
    target_link_libraries(update_client_test PRIVATE winhttp advapi32 ws2_32)
    add_test(NAME update_client COMMAND update_client_test)
    set_tests_properties(update_client PROPERTIES TIMEOUT 90)
    add_executable(update_installer_test tests/integration/UpdateInstallerTest.cpp
        src/update/UpdateInstaller.cpp src/update/UpdateManifest.cpp)
    serialctl_configure_native_target(update_installer_test CONSOLE)
    target_compile_definitions(update_installer_test PRIVATE SERIALCTL_UPDATE_TESTING)
    target_link_libraries(update_installer_test PRIVATE advapi32 shell32 user32)
    add_test(NAME update_installer COMMAND update_installer_test)
    set_tests_properties(update_installer PROPERTIES TIMEOUT 90)
    add_executable(session_logger_test tests/integration/SessionLoggerTest.cpp src/logging/SessionLogger.cpp)
    serialctl_configure_native_target(session_logger_test CONSOLE)
    target_compile_definitions(session_logger_test PRIVATE SERIALCTL_LOGGER_TESTING)
    target_link_libraries(session_logger_test PRIVATE shell32 advapi32)
    add_test(NAME session_logger COMMAND session_logger_test)
    set_tests_properties(session_logger PROPERTIES TIMEOUT 90)
    add_executable(power_service_test tests/integration/PowerServiceTest.cpp src/power/PowerService.cpp src/logging/SessionLogger.cpp)
    serialctl_configure_native_target(power_service_test CONSOLE)
    target_compile_definitions(power_service_test PRIVATE SERIALCTL_POWER_TESTING)
    target_link_libraries(power_service_test PRIVATE shell32 ws2_32)
    add_test(NAME power_service COMMAND power_service_test)
    add_executable(cmd_connection_test tests/integration/CmdConnectionTest.cpp src/connections/CmdConnection.cpp src/connections/QueuedConnection.cpp src/connections/ChildProcess.cpp src/network/SessionService.cpp)
    serialctl_configure_native_target(cmd_connection_test CONSOLE)
    add_dependencies(cmd_connection_test serialctl)
    add_test(NAME cmd_connection COMMAND cmd_connection_test)
    set(GATEWAY_TEST_SOURCES src/network/ApiServer.cpp src/network/WebSocket.cpp
        src/network/GatewayClient.cpp src/network/SessionService.cpp
        src/connections/QueuedConnection.cpp src/connections/SharedSerialConnection.cpp)
    add_executable(api_server_test tests/integration/ApiServerTest.cpp ${GATEWAY_TEST_SOURCES})
    serialctl_configure_native_target(api_server_test CONSOLE)
    target_link_libraries(api_server_test PRIVATE ws2_32 advapi32)
    add_test(NAME api_server COMMAND api_server_test)
    add_executable(api_response_test tests/unit/ApiResponseTest.cpp)
    serialctl_configure_native_target(api_response_test CONSOLE)
    add_test(NAME api_response COMMAND api_response_test)
    set_tests_properties(power_service cmd_connection api_server PROPERTIES TIMEOUT 60)
    add_executable(shared_serial_protocol_test tests/integration/SharedSerialProtocolTest.cpp ${GATEWAY_TEST_SOURCES})
    serialctl_configure_native_target(shared_serial_protocol_test CONSOLE)
    target_link_libraries(shared_serial_protocol_test PRIVATE ws2_32 advapi32)
    add_test(NAME shared_serial_protocol COMMAND shared_serial_protocol_test)

    add_executable(udp_serial_test tests/integration/UdpSerialConnectionTest.cpp src/connections/UdpSerialConnection.cpp)
    serialctl_configure_native_target(udp_serial_test CONSOLE)
    target_link_libraries(udp_serial_test PRIVATE ws2_32)
    add_test(NAME udp_serial COMMAND udp_serial_test)

    add_executable(tcp_connection_test
        tests/integration/TcpConnectionTest.cpp
        src/connections/TcpConnection.cpp
    )
    serialctl_configure_native_target(tcp_connection_test CONSOLE)
    target_link_libraries(tcp_connection_test PRIVATE ws2_32)
    add_test(NAME tcp_connection COMMAND tcp_connection_test)

    add_executable(terminal_model_test
        tests/unit/TerminalModelTest.cpp
        src/terminal/TerminalModel.cpp
    )
    serialctl_configure_native_target(terminal_model_test CONSOLE)
    add_test(NAME terminal_model COMMAND terminal_model_test)

    add_executable(putty_host_key_test tests/integration/PuttyHostKeyTest.cpp
        src/connections/PuttyHostKey.cpp src/connections/ChildProcess.cpp src/connections/PasswordPipe.cpp)
    serialctl_configure_native_target(putty_host_key_test CONSOLE)
    target_link_libraries(putty_host_key_test PRIVATE advapi32 ole32)
    add_test(NAME putty_host_key COMMAND putty_host_key_test)

    add_executable(serial_share_backpressure_test tests/integration/SerialShareBackpressureTest.cpp ${GATEWAY_TEST_SOURCES})
    serialctl_configure_native_target(serial_share_backpressure_test CONSOLE)
    target_link_libraries(serial_share_backpressure_test PRIVATE ws2_32 advapi32 psapi)
    add_test(NAME serial_share_backpressure COMMAND serial_share_backpressure_test)
    add_executable(multi_serial_test tests/integration/MultiSerialTest.cpp ${GATEWAY_TEST_SOURCES})
    serialctl_configure_native_target(multi_serial_test CONSOLE)
    target_link_libraries(multi_serial_test PRIVATE ws2_32 advapi32)
    add_test(NAME multi_serial COMMAND multi_serial_test)
    add_executable(shared_terminal_response_test tests/integration/SharedTerminalResponseTest.cpp
        src/terminal/TerminalModel.cpp ${GATEWAY_TEST_SOURCES})
    serialctl_configure_native_target(shared_terminal_response_test CONSOLE)
    target_link_libraries(shared_terminal_response_test PRIVATE ws2_32 advapi32)
    add_test(NAME shared_terminal_response COMMAND shared_terminal_response_test)
    set_tests_properties(shared_terminal_response PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME api_client COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_api_client.py")
    set_tests_properties(api_client PROPERTIES TIMEOUT 30)
    add_test(NAME linux_gateway_client COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_gateway_client.py"
        "$<TARGET_FILE:multi_serial_test>")
    set_tests_properties(linux_gateway_client PROPERTIES TIMEOUT 30)

    add_executable(terminal_decoder_test tests/unit/TerminalDecoderTest.cpp)
    serialctl_configure_native_target(terminal_decoder_test CONSOLE)
    add_test(NAME terminal_decoder COMMAND terminal_decoder_test)

    add_executable(sftp_model_test
        tests/unit/SftpModelTest.cpp
        src/sftp/SftpModel.cpp
    )
    serialctl_configure_native_target(sftp_model_test CONSOLE)
    add_test(NAME sftp_model COMMAND sftp_model_test)
    set_tests_properties(multi_serial shared_serial_protocol tcp_connection terminal_model terminal_decoder sftp_model putty_host_key PROPERTIES TIMEOUT 60)
    set_tests_properties(serial_share_backpressure PROPERTIES TIMEOUT 120)
endif()

if(BUILD_TESTING)
    set_tests_properties(api_server multi_serial shared_serial_protocol serial_share_backpressure linux_gateway_client PROPERTIES RUN_SERIAL TRUE)
endif()
