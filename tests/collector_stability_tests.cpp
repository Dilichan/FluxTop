#include <winsock2.h>
#include <ws2tcpip.h>
#include "fluxtop/sampler.hpp"
#include "fluxtop/output.hpp"
#include "fluxtop/windows.hpp"
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
using namespace std::chrono_literals;
namespace {
void need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void percent(const std::optional<double>& value){need(!value || (std::isfinite(*value) && *value>=0 && *value<=100.001),"invalid percentage");}
void temperature(const std::optional<double>& value){need(!value || (std::isfinite(*value) && *value>0 && *value<150),"invalid zero/stale temperature value");}
struct Workload {
    WSADATA data{};SOCKET tx{INVALID_SOCKET},rx{INVALID_SOCKET};HANDLE file{INVALID_HANDLE_VALUE};
    std::atomic<bool> failed{};
    std::jthread cpu,io;
    explicit Workload(const std::filesystem::path& path){
        need(WSAStartup(MAKEWORD(2,2),&data)==0,"Winsock startup");
        rx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);tx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);need(rx!=INVALID_SOCKET && tx!=INVALID_SOCKET,"owned loopback sockets");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        need(bind(rx,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"bind loopback");int length=sizeof(address);
        need(getsockname(rx,reinterpret_cast<sockaddr*>(&address),&length)==0,"loopback port");u_long nonblock=1;ioctlsocket(rx,FIONBIO,&nonblock);
        file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_WRITE_THROUGH,nullptr);
        need(file!=INVALID_HANDLE_VALUE,"owned disk workload");
        cpu=std::jthread([](std::stop_token stop){while(!stop.stop_requested()){const auto until=std::chrono::steady_clock::now()+2ms;while(std::chrono::steady_clock::now()<until)YieldProcessor();std::this_thread::sleep_for(2ms);}});
        io=std::jthread([this,address](std::stop_token stop){char bytes[65536]{};unsigned offset{};
            while(!stop.stop_requested()){
                DWORD written{};if(!WriteFile(file,bytes,sizeof(bytes),&written,nullptr) || written!=sizeof(bytes))failed=true;
                if(++offset==128){LARGE_INTEGER zero{};SetFilePointerEx(file,zero,nullptr,FILE_BEGIN);offset=0;}
                if(sendto(tx,bytes,4096,0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=4096)failed=true;
                recv(rx,bytes,sizeof(bytes),0);std::this_thread::sleep_for(10ms);
            }});
    }
    ~Workload(){io.request_stop();cpu.request_stop();if(io.joinable())io.join();if(cpu.joinable())cpu.join();if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(tx!=INVALID_SOCKET)closesocket(tx);if(rx!=INVALID_SOCKET)closesocket(rx);WSACleanup();}
};
}
int wmain(int argc,wchar_t** argv){try{
    if(!fluxtop::initialize_privileges().elevated.value_or(false))return 77;
    need(argc>=2,"fixture root required");wchar_t configured[16]{};GetEnvironmentVariableW(L"FLUXTOP_STABILITY_SAMPLES",configured,16);
    unsigned samples=argc>2?static_cast<unsigned>(_wtoi(argv[2])):(configured[0]?static_cast<unsigned>(_wtoi(configured)):12);
    need(samples>=4 && samples<=7200,"samples must be 4..7200");
    const auto root=std::filesystem::path(argv[1])/(L"collector-soak-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);SetEnvironmentVariableW(L"FLUXTOP_DATA_DB",(root/L"history.db").c_str());
    DWORD baseline{};std::uint64_t previous_time{};bool own_cpu=false,cpu_etw=false,disk=false,network=false;
    {Workload load(root/L"workload.bin");
    for(unsigned cycle=0;cycle<3;++cycle){
        {fluxtop::Sampler sampler(true,true,true);sampler.set_fps_targets({GetCurrentProcessId()});
            for(unsigned i=0;i<samples;++i){std::this_thread::sleep_for(500ms);const auto s=sampler.collect();
                need(s.elevated.value_or(false),"all collectors run elevated");need(!s.processes.empty() && s.logical_processors>0,"process list and CPU topology");
                need(s.timestamp_ms>static_cast<std::int64_t>(previous_time),"measurement timestamps advance");previous_time=static_cast<std::uint64_t>(s.timestamp_ms);
                need(s.total_ram_used && s.total_ram_capacity && *s.total_ram_used<=*s.total_ram_capacity,"system RAM is valid");
                percent(s.total_cpu_percent);percent(s.total_gpu_percent);temperature(s.cpu_temperature_c);temperature(s.gpu_temperature_c);
                need(s.history_error.empty(),"history continues during active collection");
                for(const auto& p:s.processes)if(p.pid==GetCurrentProcessId()){
                    percent(p.cpu_percent);percent(p.gpu_percent);need(p.working_set.has_value(),"own RAM is accessible");
                    own_cpu=own_cpu || p.cpu_percent.value_or(0)>0;
                    need(!p.presented_fps || (std::isfinite(*p.presented_fps) && *p.presented_fps>=0),"FPS validity");
                }
                cpu_etw=cpu_etw || s.cpu_execution_status=="ready" || s.cpu_execution_status=="partial";
                disk=disk || s.disk_io_status=="ready" || s.disk_io_status=="partial-attribution";
                network=network || s.network_status=="ready";
                std::ostringstream json;fluxtop::write_jsonl(json,s,100);need(json.str().find("\"processes\"")!=std::string::npos,"live JSONL remains serializable");
                if(i==samples-1){unsigned fps_known{};for(const auto& p:s.processes)if(p.presented_fps)++fps_known;
                    std::cout<<"cycle="<<cycle<<" cpu_etw="<<s.cpu_execution_status<<" disk="<<s.disk_io_status<<" network="<<s.network_status<<" gpu="<<s.gpu_status<<" temperature="<<s.cpu_temperature_source<<" dts="<<s.intel_core_temperatures.status<<" cores_available="<<s.intel_core_temperatures.available_cores<<" fps_known="<<fps_known<<" working_set="<<s.self_working_set.value_or(0)<<" handles="<<s.self_handles.value_or(0)<<'\n';}
            }}
        std::this_thread::sleep_for(200ms);DWORD handles{};need(GetProcessHandleCount(GetCurrentProcess(),&handles)!=FALSE,"handle count");
        if(cycle==0)baseline=handles;else need(handles<=baseline+8,"collector restart leaks handles");
    }
    need(!load.failed,"owned network/disk workload failed");need(own_cpu && cpu_etw && disk && network,"CPU, disk and network must become operational together");}
    std::filesystem::remove_all(root);
    std::cout<<"Joint collector lifecycle passed; unavailable optional GPU/FPS/sensor providers remain explicit\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
