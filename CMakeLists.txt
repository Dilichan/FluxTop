cmake_minimum_required(VERSION 3.24)
project(FluxTop VERSION 0.2.1 LANGUAGES CXX RC)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(FLUXTOP_RELEASE_TAG "v${PROJECT_VERSION}" CACHE STRING "GitHub release tag embedded in the downloadable launcher")

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FluxTop requires 64-bit Windows and a Windows SDK")
endif()

add_library(fluxtop_core src/model.cpp src/options.cpp src/output.cpp)
target_include_directories(fluxtop_core PUBLIC include)
target_compile_features(fluxtop_core PUBLIC cxx_std_20)
set_target_properties(fluxtop_core PROPERTIES CXX_EXTENSIONS OFF)

add_library(fluxtop_icons src/app_icons.cpp)
target_include_directories(fluxtop_icons PUBLIC include)
target_compile_features(fluxtop_icons PUBLIC cxx_std_20)
target_compile_definitions(fluxtop_icons PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(fluxtop_icons PUBLIC shell32 user32 ole32 windowscodecs winsqlite3)

configure_file(src/resources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/fluxtop.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/fluxtop.rc" PROPERTIES
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/branding/fluxtop.ico")
add_library(fluxtop_history src/history.cpp)
target_include_directories(fluxtop_history PUBLIC include)
target_compile_features(fluxtop_history PUBLIC cxx_std_20)
target_compile_definitions(fluxtop_history PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(fluxtop_history PUBLIC winsqlite3)
add_executable(fluxtop src/main.cpp src/process_collector.cpp src/gpu_collector.cpp src/privileges.cpp src/sampler.cpp src/overlay.cpp src/network_collector.cpp src/autostart.cpp src/temperatures.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/fluxtop.rc")
target_link_libraries(fluxtop PRIVATE fluxtop_core fluxtop_icons fluxtop_history psapi pdh advapi32 d2d1 dwrite shell32 user32 gdi32 ole32 comctl32 tdh uuid)
target_compile_definitions(fluxtop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 FLUXTOP_VERSION="${PROJECT_VERSION}")

set(launcher_resource "${CMAKE_CURRENT_BINARY_DIR}/launcher-$<CONFIG>.rc")
add_library(fluxtop_release_update src/release_update.cpp)
target_include_directories(fluxtop_release_update PUBLIC include)
target_compile_features(fluxtop_release_update PUBLIC cxx_std_20)
target_compile_definitions(fluxtop_release_update PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(fluxtop_release_update PUBLIC winhttp bcrypt runtimeobject shell32)
add_custom_command(OUTPUT "${launcher_resource}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:fluxtop>" "-DOUTPUT=${launcher_resource}"
        "-DICON=${PROJECT_SOURCE_DIR}/assets/branding/fluxtop.ico" "-DVERSION=${PROJECT_VERSION}"
        "-DRELEASE_TAG=${FLUXTOP_RELEASE_TAG}" "-DTEMPLATE=${PROJECT_SOURCE_DIR}/src/launcher.rc.in"
        -P "${PROJECT_SOURCE_DIR}/tools/Generate-LauncherResource.cmake"
    DEPENDS fluxtop "${PROJECT_SOURCE_DIR}/assets/branding/fluxtop.ico" "${PROJECT_SOURCE_DIR}/tools/Generate-LauncherResource.cmake" "${PROJECT_SOURCE_DIR}/src/launcher.rc.in"
    VERBATIM)
add_executable(fluxtop_launcher WIN32 src/launcher.cpp "${launcher_resource}")
target_include_directories(fluxtop_launcher PRIVATE include)
target_compile_features(fluxtop_launcher PRIVATE cxx_std_20)
target_compile_definitions(fluxtop_launcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(fluxtop_launcher PRIVATE fluxtop_release_update shell32 user32 ole32 uuid)
set_target_properties(fluxtop_launcher PROPERTIES OUTPUT_NAME FluxTop
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/$<CONFIG>")

include(CTest)
if(BUILD_TESTING)
    add_executable(fluxtop_tests tests/core_tests.cpp)
    target_link_libraries(fluxtop_tests PRIVATE fluxtop_core)
    add_test(NAME core COMMAND fluxtop_tests)
    add_executable(fluxtop_release_tests tests/release_tests.cpp)
    target_link_libraries(fluxtop_release_tests PRIVATE fluxtop_release_update)
    target_compile_definitions(fluxtop_release_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME release_update COMMAND fluxtop_release_tests "$<TARGET_FILE:fluxtop_launcher>" "${CMAKE_CURRENT_BINARY_DIR}" "${FLUXTOP_RELEASE_TAG}")
    add_test(NAME portable COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tests/portable_tests.ps1" -Launcher "$<TARGET_FILE:fluxtop_launcher>"
        -Monitor "$<TARGET_FILE:fluxtop>" -FixtureRoot "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(release_update portable PROPERTIES TIMEOUT 30)
    add_executable(fluxtop_history_tests tests/history_tests.cpp)
    target_link_libraries(fluxtop_history_tests PRIVATE fluxtop_history)
    add_test(NAME history COMMAND fluxtop_history_tests "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(history PROPERTIES TIMEOUT 20)
    add_executable(fluxtop_network_tests tests/network_tests.cpp src/network_collector.cpp)
    target_link_libraries(fluxtop_network_tests PRIVATE fluxtop_core advapi32 ole32 tdh ws2_32)
    target_compile_definitions(fluxtop_network_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    add_test(NAME network COMMAND fluxtop_network_tests)
    set_tests_properties(network PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 40)
    add_executable(fluxtop_icon_tests tests/icon_cache_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/fluxtop.rc")
    target_link_libraries(fluxtop_icon_tests PRIVATE fluxtop_icons)
    target_compile_definitions(fluxtop_icon_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    add_test(NAME icon_cache COMMAND fluxtop_icon_tests "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(icon_cache PROPERTIES TIMEOUT 45)
    add_test(NAME cli_help COMMAND fluxtop --help)
    set_tests_properties(cli_help PROPERTIES TIMEOUT 20)
    add_test(NAME cli_invalid COMMAND fluxtop --interval-ms 0)
    set_tests_properties(cli_invalid PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(cli_invalid PROPERTIES TIMEOUT 20)
    add_test(NAME smoke COMMAND fluxtop --samples 2 --interval-ms 250 --no-gpu --jsonl)
    set_tests_properties(smoke PROPERTIES TIMEOUT 20 ENVIRONMENT "FLUXTOP_HISTORY_DB=${CMAKE_CURRENT_BINARY_DIR}/smoke-history.db")
    add_test(NAME overlay_smoke COMMAND fluxtop --overlay --samples 2 --interval-ms 250 --no-gpu --top 5)
    set_tests_properties(overlay_smoke PROPERTIES TIMEOUT 20
        ENVIRONMENT "FLUXTOP_OVERLAY_SETTINGS=${CMAKE_CURRENT_BINARY_DIR}/overlay-smoke.ini;FLUXTOP_ICON_DB=${CMAKE_CURRENT_BINARY_DIR}/overlay-icons.db;FLUXTOP_HISTORY_DB=${CMAKE_CURRENT_BINARY_DIR}/overlay-history.db")
endif()

foreach(target fluxtop fluxtop_launcher fluxtop_release_update fluxtop_core fluxtop_icons fluxtop_history fluxtop_tests fluxtop_icon_tests fluxtop_network_tests fluxtop_history_tests fluxtop_release_tests)
    if(TARGET ${target} AND MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8;/EHsc>")
    endif()
endforeach()

install(TARGETS fluxtop RUNTIME DESTINATION bin)
