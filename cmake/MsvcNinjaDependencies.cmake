# CMake may decode localized /showIncludes using OEM rather than Windows ANSI.
set(FLUXTOP_PREFIX_PROBE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/Get-MsvcIncludesPrefix.ps1")
macro(fluxtop_configure_compiler_environment)
    if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
        set(_fluxtop_probe "${CMAKE_BINARY_DIR}/CMakeFiles/fluxtop-prefix-probe")
        file(WRITE "${_fluxtop_probe}.hpp" "\n")
        file(WRITE "${_fluxtop_probe}.cpp" "#include \"fluxtop-prefix-probe.hpp\"\n")
        execute_process(COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${FLUXTOP_PREFIX_PROBE_SCRIPT}" -CompilerPath "${CMAKE_CXX_COMPILER}" -ProbePath "${_fluxtop_probe}.cpp"
            ENCODING UTF-8 OUTPUT_VARIABLE _fluxtop_prefix ERROR_VARIABLE _fluxtop_error RESULT_VARIABLE _fluxtop_result)
        if(NOT _fluxtop_result EQUAL 0 OR NOT _fluxtop_prefix)
            message(FATAL_ERROR "Cannot detect MSVC header dependency prefix: ${_fluxtop_error}")
        endif()
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_fluxtop_prefix}")
        # FluxTop has no C++20 modules and needs no independent scanner.
        set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    endif()
endmacro()
