#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/network_collector.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/windows.hpp"
#include <evntrace.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
namespace {
void need(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
struct Properties {EVENT_TRACE_PROPERTIES p{};wchar_t name[1024]{};};
std::vector<std::wstring> sessions(DWORD pid=GetCurrentProcessId()){
    std::vector<Properties> values(128);std::vector<EVENT_TRACE_PROPERTIES*> pointers;
    for(auto& value:values){value.p.Wnode.BufferSize=sizeof(Properties);value.p.LoggerNameOffset=offsetof(Properties,name);pointers.push_back(&value.p);}
    ULONG count{};need(QueryAllTracesW(pointers.data(),static_cast<ULONG>(pointers.size()),&count)==ERROR_SUCCESS,"enumerate sessions");
    std::vector<std::wstring> result;const auto id=std::to_wstring(pid)+L".";
    for(ULONG i=0;i<std::min<ULONG>(count,static_cast<ULONG>(values.size()));++i){const auto& value=values[i];
        if(!value.p.LoggerNameOffset || value.p.LoggerNameOffset>=sizeof(Properties))continue;
        const auto* name=reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(&value)+value.p.LoggerNameOffset);
        const std::wstring_view text(name);
        if(text.starts_with(L"FluxTop.Disk."+id) || text.starts_with(L"FluxTop.Network."+id))result.emplace_back(text);
    }return result;
}
void stop_owned(const std::vector<std::wstring>& names){
    const auto id=std::to_wstring(GetCurrentProcessId())+L".";
    for(const auto& name:names){
        need(name.starts_with(L"FluxTop.Disk."+id) || name.starts_with(L"FluxTop.Network."+id),"refuse to stop any foreign session");
        Properties p{};p.p.Wnode.BufferSize=sizeof(p);p.p.Wnode.Flags=WNODE_FLAG_TRACED_GUID;p.p.LoggerNameOffset=offsetof(Properties,name);
        need(ControlTraceW(0,name.c_str(),&p.p,EVENT_TRACE_CONTROL_STOP)==ERROR_SUCCESS,"stop only the current fixture's session");
    }
}
fluxtop::Snapshot snapshot(){
    fluxtop::Snapshot s;s.total_cpu_percent=17;fluxtop::Process p;p.pid=GetCurrentProcessId();p.has_baseline=true;
    FILETIME birth{},end{},kernel{},user{};need(GetProcessTimes(GetCurrentProcess(),&birth,&end,&kernel,&user)!=FALSE,"fixture identity");
    p.creation_time=fluxtop::filetime_ticks(birth);p.cpu_percent=42;p.gpu_percent=23;p.working_set=1024;p.presented_fps=60;p.auto_scores.fill(7);
    s.processes.push_back(p);return s;
}
bool ready(const fluxtop::Snapshot& s){return (s.cpu_execution_status=="ready" || s.cpu_execution_status=="partial") &&
    s.cpu_execution_end_qpc>s.cpu_execution_begin_qpc && s.disk_io_status!="incomplete" && s.network_status=="ready";}
void poll(fluxtop::DiskIoCollector& disk,fluxtop::NetworkCollector& net,fluxtop::Snapshot& s){
    disk.collect(s);net.collect(s);
    need(s.total_cpu_percent==17 && s.processes[0].cpu_percent==42 && s.processes[0].gpu_percent==23 &&
        s.processes[0].working_set==1024 && s.processes[0].presented_fps==60 && s.processes[0].auto_scores[0]==7,
        "one ETW failure must not alter other measurements or AutoScore");
}
void policy(){
    using fluxtop::EtwRecovery;const auto start=EtwRecovery::Clock::time_point{};
    EtwRecovery r;need(!r.due(start) && r.schedule(start) && !r.schedule(start+1s),"one scheduled recovery, not one per sample");
    need(!r.due(start+2s) && r.due(start+3s),"first recovery is delayed, never a busy loop");
    r.restarted(start+3s);r.schedule(start+3s);need(!r.due(start+8s) && r.due(start+9s),"repeated failures back off");
    r.restarted(start+9s);r.healthy(start+70s);r.schedule(start+70s);need(r.due(start+73s),"stable operation resets retry pressure");
    auto now=start+73s;for(unsigned i=0;i<20;++i){r.restarted(now);need(r.schedule(now),"schedule after another failed session");
        need(!r.due(now+2s) && r.due(now+60s),"persistent failures retain delay and never exceed one minute");now+=60s;}
}
void interrupted(){
    DWORD baseline{};
    {fluxtop::DiskIoCollector warm;fluxtop::NetworkCollector net(true);std::this_thread::sleep_for(200ms);}
    need(GetProcessHandleCount(GetCurrentProcess(),&baseline)!=FALSE,"handle baseline");
    for(unsigned cycle=0;cycle<2;++cycle){
        fluxtop::DiskIoCollector disk;fluxtop::NetworkCollector net(true);auto s=snapshot();
        const auto before=sessions();need(before.size()==2,"one CPU/disk session and one network session");
        bool initial=false;for(unsigned i=0;i<12 && !initial;++i){std::this_thread::sleep_for(500ms);poll(disk,net,s);initial=ready(s);}
        need(initial,"collect a complete initial trace window");stop_owned(before);
        s.processes[0].rx_bytes_per_second=999;s.processes[0].disk_read_bytes_per_second=999;
        poll(disk,net,s);
        need(!s.processes[0].rx_bytes_per_second && !s.processes[0].disk_read_bytes_per_second,"interrupted collectors must clear stale rates");
        need(s.cpu_execution_status=="incomplete" && s.network_status=="incomplete","interruption is unavailable, never a measured zero");
        for(const auto& cpu:s.processes[0].cpu_execution.logical)need(!cpu.percent,"lost trace cannot be exact");
        bool recovered=false;for(unsigned i=0;i<24 && !recovered;++i){std::this_thread::sleep_for(500ms);poll(disk,net,s);recovered=ready(s) && sessions()!=before;}
        need(recovered && sessions().size()==2,"same collectors must replace stopped sessions and deliver fresh windows");
    }
    need(sessions().empty(),"normal shutdown leaves no owned sessions");
    DWORD handles{};need(GetProcessHandleCount(GetCurrentProcess(),&handles)!=FALSE && handles<=baseline+4,"recovery must not leak process handles");
}
void crash(const std::filesystem::path& root){
    wchar_t executable[32768]{};need(GetModuleFileNameW(nullptr,executable,32768)>0,"fixture executable");
    const auto marker=root/(L"network-crash-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".ready");
    auto command=L"\""+std::wstring(executable)+L"\" --crash-child \""+marker.wstring()+L"\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION p{};
    need(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&p)!=FALSE,"start owned crash child");
    struct Cleanup{PROCESS_INFORMATION& p;~Cleanup(){if(WaitForSingleObject(p.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(p.hProcess,54);WaitForSingleObject(p.hProcess,5000);}CloseHandle(p.hProcess);CloseHandle(p.hThread);}}cleanup{p};
    for(unsigned i=0;i<100 && !std::filesystem::exists(marker);++i)std::this_thread::sleep_for(50ms);
    need(std::filesystem::exists(marker) && sessions(p.dwProcessId).size()==2,"child owns both sessions before the crash");
    need(TerminateProcess(p.hProcess,54)!=FALSE && WaitForSingleObject(p.hProcess,5000)==WAIT_OBJECT_0,"terminate only the owned child");
    // Keep its process handle open: an exited process object still exists.
    {fluxtop::DiskIoCollector disk;fluxtop::NetworkCollector net(true);
        need(sessions(p.dwProcessId).empty(),"dead-owner sessions must be reclaimed even while its process handle is held");}
    std::error_code ec;std::filesystem::remove(marker,ec);
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==2 && std::wstring_view(argv[1])==L"--policy"){policy();return 0;}
    if(!fluxtop::initialize_privileges().elevated.value_or(false)){std::cout<<"Administrator required\n";return 77;}
    if(argc==3 && std::wstring_view(argv[1])==L"--crash-child"){
        fluxtop::DiskIoCollector disk;fluxtop::NetworkCollector net(true);{std::ofstream marker(argv[2]);marker<<"ready";}Sleep(INFINITE);return 1;
    }
    need(argc==2,"fixture directory required");policy();interrupted();crash(argv[1]);
    need(sessions().empty(),"crash/restart cleanup leaves no sessions");
    std::cout<<"CPU/disk/network interruption, automatic recovery, crash cleanup and handle stability passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
