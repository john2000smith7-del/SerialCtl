# Included only in temporary benchmark worktrees, never in the release build.
get_target_property(SERIALCTL_PERF_APP_SOURCES serialctl SOURCES)
list(REMOVE_ITEM SERIALCTL_PERF_APP_SOURCES src/app/App.cpp "${YY_THUNKS_OBJECT}")
add_executable(serialctl_perf_paint "${SERIALCTL_PERF_DRIVER_DIR}/MainWindowPaintBenchmark.cpp" ${SERIALCTL_PERF_APP_SOURCES})
serialctl_configure_native_target(serialctl_perf_paint CONSOLE)
target_compile_definitions(serialctl_perf_paint PRIVATE SERIALCTL_AI_TESTING SERIALCTL_POWER_TESTING
    SERIALCTL_VERSION=L"${SERIALCTL_CPP_VERSION}" SERIALCTL_APP_VERSION="${SERIALCTL_CPP_VERSION}")
target_link_libraries(serialctl_perf_paint PRIVATE ws2_32 setupapi msimg32 comctl32 comdlg32 shell32 ole32 advapi32 gdiplus winhttp psapi)
if(MSVC)
    target_link_options(serialctl_perf_paint PRIVATE /MANIFEST:NO)
endif()
add_executable(serialctl_perf_preview "${SERIALCTL_PERF_DRIVER_DIR}/PreviewBenchmark.cpp" src/terminal/TerminalModel.cpp)
serialctl_configure_native_target(serialctl_perf_preview CONSOLE)
target_link_libraries(serialctl_perf_preview PRIVATE psapi)
