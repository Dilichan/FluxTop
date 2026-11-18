cmake_minimum_required(VERSION 3.24)
project(FluxTop VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FluxTop requires 64-bit Windows and a Windows SDK")
endif()

add_library(fluxtop_core src/model.cpp src/options.cpp src/output.cpp)
target_include_directories(fluxtop_core PUBLIC include)
target_compile_features(fluxtop_core PUBLIC cxx_std_20)
set_target_properties(fluxtop_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(fluxtop src/main.cpp src/process_collector.cpp src/gpu_collector.cpp)
target_link_libraries(fluxtop PRIVATE fluxtop_core psapi pdh)
target_compile_definitions(fluxtop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 FLUXTOP_VERSION="${PROJECT_VERSION}")

include(CTest)
if(BUILD_TESTING)
    add_executable(fluxtop_tests tests/core_tests.cpp)
    target_link_libraries(fluxtop_tests PRIVATE fluxtop_core)
    add_test(NAME core COMMAND fluxtop_tests)
    add_test(NAME cli_help COMMAND fluxtop --help)
    add_test(NAME cli_invalid COMMAND fluxtop --interval-ms 0)
    set_tests_properties(cli_invalid PROPERTIES WILL_FAIL TRUE)
    add_test(NAME smoke COMMAND fluxtop --samples 2 --interval-ms 250 --no-gpu --jsonl)
    set_tests_properties(smoke PROPERTIES TIMEOUT 20)
endif()

foreach(target fluxtop fluxtop_core fluxtop_tests)
    if(TARGET ${target} AND MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    endif()
endforeach()

install(TARGETS fluxtop RUNTIME DESTINATION bin)
