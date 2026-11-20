#include "fluxtop/diagnostics.hpp"
#include <windows.h>
#include <cstdio>
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
    // A fresh event must carry the current PC wall time and an explicit UTC offset.
    SYSTEMTIME before{},after{};
    GetLocalTime(&before);
    fluxtop::diagnostics::write(fluxtop::diagnostics::Level::info,"timestamp-check","local clock");
    fluxtop::diagnostics::flush();
    GetLocalTime(&after);
    std::ifstream stream(path,std::ios::binary);
    if(!stream)return 6;
    std::string line,probe;
    while(std::getline(stream,line))
        if(line.find("timestamp-check | local clock")!=std::string::npos)probe=line;
    if(probe.empty())return 7;
    const auto local_prefix=[](const SYSTEMTIME& time){
        char buffer[20]{};
        std::snprintf(buffer,sizeof(buffer),"%04u-%02u-%02uT%02u:%02u:%02u",
            static_cast<unsigned>(time.wYear),static_cast<unsigned>(time.wMonth),
            static_cast<unsigned>(time.wDay),static_cast<unsigned>(time.wHour),
            static_cast<unsigned>(time.wMinute),static_cast<unsigned>(time.wSecond));
        return std::string(buffer);
    };
    if(probe.compare(0,19,local_prefix(before))!=0 &&
       probe.compare(0,19,local_prefix(after))!=0)return 8;
    if(probe.size()<35 || probe[19]!='.' ||
       (probe[23]!='+' && probe[23]!='-') || probe[26]!=':' || probe[29]!=' ' ||
       probe.compare(30,4,"pid=")!=0)return 9;
    std::cout<<"Structured bounded journal, local timestamps and rotation fixture passed\n";
    return 0;
}
