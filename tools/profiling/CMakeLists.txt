cmake_minimum_required(VERSION 3.24)
project(FluxTopProfile LANGUAGES CXX RC)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}" app)
include("${ROOT}/cmake/MsvcNinjaDependencies.cmake")
fluxtop_configure_compiler_environment()
add_executable(profile profile.cpp
 process_probe.cpp "${ROOT}/src/process_enumerator.cpp" "${ROOT}/src/gpu_collector.cpp" "${ROOT}/src/fps_collector.cpp" "${ROOT}/src/privileges.cpp"
 "${ROOT}/src/network_collector.cpp" "${ROOT}/src/disk_collector.cpp" "${ROOT}/src/disk_temperature.cpp" "${ROOT}/src/disk_io_collector.cpp"
 "${ROOT}/src/cpu_etw_collector.cpp" "${ROOT}/src/temperatures.cpp" "${ROOT}/src/temperature_selection.cpp" "${ROOT}/src/intel_dts.cpp" "${ROOT}/src/cpu_logical_collector.cpp")
target_compile_features(profile PRIVATE cxx_std_20)
target_include_directories(profile PRIVATE "${ROOT}/include")
target_compile_definitions(profile PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(profile PRIVATE /O2 /W4 /WX /utf-8 /EHsc)
target_link_libraries(profile PRIVATE fluxtop_core fluxtop_history psapi pdh advapi32 ole32 oleaut32 wbemuuid tdh)
add_custom_command(TARGET profile POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/assets/pawnio/IntelMSR.bin" "$<TARGET_FILE_DIR:profile>/IntelMSR.bin")


add_executable(enumeration_benchmark enumeration_benchmark.cpp)
target_compile_features(enumeration_benchmark PRIVATE cxx_std_20)
target_include_directories(enumeration_benchmark PRIVATE "${ROOT}/include")
target_compile_definitions(enumeration_benchmark PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(enumeration_benchmark PRIVATE /O2 /W4 /WX /utf-8 /EHsc)
