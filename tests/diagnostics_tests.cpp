#include "fluxtop/diagnostics.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const auto directory=std::filesystem::path(argv[1])/
        (L"diagnostics-test-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto path=directory/L"activity.log";
    if(!SetEnvironmentVariableW(L"FLUXTOP_LOG_PATH",path.c_str()))return 2;
    if(!SetEnvironmentVariableW(L"FLUXTOP_LOG_TEST_SEGMENT_KIB",L"64"))return 3;
    for(int i=0;i<3000;++i)
        fluxtop::diagnostics::write(fluxtop::diagnostics::Level::info,"fixture",std::string(270,'x'));
    fluxtop::diagnostics::flush();
    std::uint64_t total=0;
    unsigned files=0;
    for(unsigned i=0;i<5;++i){
        auto p=path;
        if(i)p+=L"."+std::to_wstring(i);
        std::error_code error;
        if(!std::filesystem::exists(p,error))continue;
        const auto size=std::filesystem::file_size(p,error);
        if(error || size>64ULL*1024)return 4;
        total+=size;++files;
    }
    if(!files || total>5ULL*64*1024)return 5;
    std::cout<<"Structured bounded journal and rotation fixture passed\n";
    return 0;
}
