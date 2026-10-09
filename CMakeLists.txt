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

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SERIALCTL_ARCH "x64")
    set(PUTTY_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/putty/prebuilt/x64")
else()
    set(SERIALCTL_ARCH "x86")
    set(PUTTY_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/putty/prebuilt/x86")
endif()

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
    "${SERIALCTL_SOURCE_DIR}/connections"
    "${SERIALCTL_SOURCE_DIR}/logging"
    "${SERIALCTL_SOURCE_DIR}/platform"
    "${SERIALCTL_SOURCE_DIR}/sftp"
    "${SERIALCTL_SOURCE_DIR}/terminal"
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
    src/connections/ChildProcess.cpp
    src/connections/PlinkConnection.cpp
    src/connections/PuttyHostKey.cpp
    src/connections/SerialConnection.cpp
    src/connections/SerialDevice.cpp
    src/connections/SerialShareConnection.cpp
    src/connections/SharedSerialConnection.cpp
    src/connections/TcpConnection.cpp
    src/logging/SessionLogger.cpp
    src/sftp/SftpClient.cpp
    src/sftp/SftpModel.cpp
    src/terminal/TerminalModel.cpp
    "${SERIALCTL_GENERATED_DIR}/serialctl.rc"
)
serialctl_configure_native_target(serialctl WINDOWS)
target_compile_definitions(serialctl PRIVATE SERIALCTL_VERSION=L"${SERIALCTL_CPP_VERSION}")
if(MSVC)
    # serialctl.rc embeds the generated, version-controlled application manifest.
    # Disable the linker-generated default manifest to avoid duplicate RT_MANIFEST
    # resources and to ensure the Win7 compatibility declarations stay authoritative.
    target_link_options(serialctl PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(serialctl PRIVATE ws2_32 comctl32 comdlg32 shell32 ole32 advapi32)

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
    add_executable(shared_serial_protocol_test
        tests/integration/SharedSerialProtocolTest.cpp
        src/connections/SharedSerialConnection.cpp
    )
    serialctl_configure_native_target(shared_serial_protocol_test CONSOLE)
    target_link_libraries(shared_serial_protocol_test PRIVATE ws2_32)
    add_test(NAME shared_serial_protocol COMMAND shared_serial_protocol_test)

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
        src/connections/PuttyHostKey.cpp src/connections/ChildProcess.cpp)
    serialctl_configure_native_target(putty_host_key_test CONSOLE)
    target_link_libraries(putty_host_key_test PRIVATE advapi32)
    add_test(NAME putty_host_key COMMAND putty_host_key_test)

    add_executable(serial_share_backpressure_test tests/integration/SerialShareBackpressureTest.cpp
        src/connections/SerialShareConnection.cpp)
    serialctl_configure_native_target(serial_share_backpressure_test CONSOLE)
    target_link_libraries(serial_share_backpressure_test PRIVATE ws2_32)
    add_test(NAME serial_share_backpressure COMMAND serial_share_backpressure_test)

    add_executable(terminal_decoder_test tests/unit/TerminalDecoderTest.cpp)
    serialctl_configure_native_target(terminal_decoder_test CONSOLE)
    add_test(NAME terminal_decoder COMMAND terminal_decoder_test)

    add_executable(sftp_model_test
        tests/unit/SftpModelTest.cpp
        src/sftp/SftpModel.cpp
    )
    serialctl_configure_native_target(sftp_model_test CONSOLE)
    add_test(NAME sftp_model COMMAND sftp_model_test)
    set_tests_properties(shared_serial_protocol tcp_connection terminal_model terminal_decoder sftp_model putty_host_key serial_share_backpressure PROPERTIES TIMEOUT 30)
endif()
