#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/windows.hpp"
#include <evntrace.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <cstring>

namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::uint64_t process_runtime_ns() {
    FILETIME birth{},end{},kernel{},user{};
    need(GetProcessTimes(GetCurrentProcess(),&birth,&end,&kernel,&user)!=FALSE,"fixture CPU clock");
    return (fluxtop::filetime_ticks(kernel)+fluxtop::filetime_ticks(user))*100;
}
fluxtop::Process self() {
    fluxtop::Process row;row.pid=GetCurrentProcessId();row.has_baseline=true;row.name="cpu_etw_fixture.exe";
    FILETIME birth{},end{},kernel{},user{};
    need(GetProcessTimes(GetCurrentProcess(),&birth,&end,&kernel,&user)!=FALSE,"process creation");
    row.creation_time=fluxtop::filetime_ticks(birth);row.cpu_percent=42;row.auto_scores.fill(7);
    return row;
}
unsigned trace_count(DWORD owner=GetCurrentProcessId()) {
    struct Properties {EVENT_TRACE_PROPERTIES p{};wchar_t name[256]{};};
    std::vector<Properties> data(64);std::vector<EVENT_TRACE_PROPERTIES*> pointers;
    for(auto& row:data){row.p.Wnode.BufferSize=sizeof(Properties);row.p.LoggerNameOffset=offsetof(Properties,name);pointers.push_back(&row.p);}
    ULONG count{};
    need(QueryAllTracesW(pointers.data(),static_cast<ULONG>(pointers.size()),&count)==ERROR_SUCCESS,"query trace sessions");
    unsigned found{};
    for(ULONG i=0;i<std::min<ULONG>(count,static_cast<ULONG>(data.size()));++i) {
        const auto& p=data[i].p;
        if(!p.LoggerNameOffset || p.LoggerNameOffset>=sizeof(Properties))continue;
        const auto* name=reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(&data[i])+p.LoggerNameOffset);
        const auto prefix=L"FluxTop.Disk."+std::to_wstring(owner)+L".";
        if(std::wstring_view(name).starts_with(prefix))++found;
    }
    return found;
}
std::filesystem::path signal_path(const std::filesystem::path& root,HANDLE process,DWORD pid) {
    FILETIME birth{},end{},kernel{},user{};
    need(GetProcessTimes(process,&birth,&end,&kernel,&user)!=FALSE,"crash fixture generation");
    return root/(L"cpu-etw-child-"+std::to_wstring(pid)+L"-"+std::to_wstring(fluxtop::filetime_ticks(birth))+L".ready");
}
void crash_and_restart(const std::filesystem::path& root) {
    std::wstring exe(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
    need(length>0 && length<exe.size(),"locate crash fixture");exe.resize(length);
    auto command=L"\""+exe+L"\" \""+root.wstring()+L"\" --crash-child";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
    need(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child)!=FALSE,"start owned crash fixture");
    const auto pid=child.dwProcessId;
    struct Cleanup { PROCESS_INFORMATION& p;~Cleanup(){
        if(p.hProcess){if(WaitForSingleObject(p.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(p.hProcess,52);WaitForSingleObject(p.hProcess,5000);}CloseHandle(p.hProcess);}
        if(p.hThread)CloseHandle(p.hThread);
    }} cleanup{child};
    const auto signal=signal_path(root,child.hProcess,pid);
    const auto deadline=GetTickCount64()+8000;
    while(!std::filesystem::exists(signal) && GetTickCount64()<deadline && WaitForSingleObject(child.hProcess,0)==WAIT_TIMEOUT)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    need(std::filesystem::exists(signal) && trace_count(pid)==1,"child must own one shared CPU/disk session");
    need(TerminateProcess(child.hProcess,52)!=FALSE && WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0,"terminate only the owned fixture process");
    CloseHandle(child.hProcess);child.hProcess=nullptr;CloseHandle(child.hThread);child.hThread=nullptr;
    {
        fluxtop::DiskIoCollector restarted;
        need(trace_count(pid)==0,"orphaned shared session reclaimed after crash");
        std::string status;
        bool completed=false;
        for(unsigned i=0;i<5;++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            fluxtop::Snapshot snapshot;snapshot.processes={self()};restarted.collect(snapshot);status=snapshot.cpu_execution_status;
            if((status=="ready" || status=="partial") && snapshot.cpu_execution_end_qpc>snapshot.cpu_execution_begin_qpc)completed=true;
        }
        need(completed && (status=="ready" || status=="partial" || status=="awaiting-window"),"CPU consumer must deliver a new complete window after an abrupt owner exit");
    }
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        const auto privileges=fluxtop::initialize_privileges();
        if(!privileges.elevated.value_or(false)){std::cout<<"Administrator required\n";return 77;}
        need(argc==2 || argc==3,"fixture directory required");
        if(argc==3 && std::wstring_view(argv[2])==L"--crash-child") {
            fluxtop::DiskIoCollector collector;
            const auto signal=signal_path(argv[1],GetCurrentProcess(),GetCurrentProcessId());
            {std::ofstream ready(signal);ready<<"ready";}
            Sleep(INFINITE);return 1;
        }
        const auto directory=std::filesystem::path(argv[1])/(L"cpu-etw-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto initial=trace_count();
        const auto cpus=GetActiveProcessorCount(0);need(cpus>=2 && cpus<=64,"two logical CPUs in group zero required");
        const unsigned a=cpus-1,b=cpus-2;
        std::atomic<unsigned long long> work{0},disk_bytes{0};
        for(unsigned cycle=0;cycle<2;++cycle) {
            fluxtop::DiskIoCollector collector;
            need(trace_count()==initial+1,"CPU and disk must share exactly one new system session");
            auto busy=[&](std::stop_token stop,bool migrate) {
                GROUP_AFFINITY affinity{};affinity.Group=0;
                affinity.Mask=KAFFINITY{1}<<a;
                if(!SetThreadGroupAffinity(GetCurrentThread(),&affinity,nullptr))return;
                unsigned long long value=1;
                auto next=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);bool other=false;
                while(!stop.stop_requested()) {
                    for(unsigned i=0;i<50000;++i)value=value*6364136223846793005ULL+1;
                    work.fetch_add(value,std::memory_order_relaxed);
                    if(migrate && std::chrono::steady_clock::now()>=next) {
                        other=!other;affinity.Mask=KAFFINITY{1}<<(other?b:a);
                        SetThreadGroupAffinity(GetCurrentThread(),&affinity,nullptr);
                        next=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
                    }
                }
            };
            std::jthread worker(busy,false),migrating(busy,true);
            std::vector<std::jthread> churn;
            if(cycle==1)for(unsigned i=0;i<4;++i)churn.emplace_back([&,i](std::stop_token stop){
                GROUP_AFFINITY affinity{};affinity.Group=0;affinity.Mask=KAFFINITY{1}<<(i%2?a:b);
                if(!SetThreadGroupAffinity(GetCurrentThread(),&affinity,nullptr))return;
                while(!stop.stop_requested()){SwitchToThread();Sleep(0);}
            });
            const auto file=directory/(L"uncached-"+std::to_wstring(cycle)+L".bin");
            std::jthread writer([&](std::stop_token stop) {
                fluxtop::Handle handle(CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,
                    FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,nullptr));
                if(!handle.valid())return;
                void* bytes=VirtualAlloc(nullptr,65536,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
                if(!bytes)return;
                std::memset(bytes,0x52,65536);unsigned packets=0;
                while(!stop.stop_requested()) {
                    DWORD written{};
                    if(!WriteFile(handle.get(),bytes,65536,&written,nullptr))break;
                    disk_bytes+=written;
                    if(++packets==128){LARGE_INTEGER zero{};SetFilePointerEx(handle.get(),zero,nullptr,FILE_BEGIN);packets=0;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                VirtualFree(bytes,0,MEM_RELEASE);
            });
            unsigned seen{},seen_a{},seen_b{};bool disk=false;double peak_cpu=0;
            const auto actual_start=process_runtime_ns();std::uint64_t observed_ns{},last_end{};
            std::ofstream evidence(directory/(L"cycle-"+std::to_wstring(cycle)+L".txt"));
            for(unsigned sample=0;sample<14;++sample) {
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                fluxtop::Snapshot snapshot;snapshot.processes={self()};snapshot.total_cpu_percent=17;
                collector.collect(snapshot);
                const auto& row=snapshot.processes[0];
                evidence<<sample<<" cpu="<<snapshot.cpu_execution_status<<" process="<<row.cpu_execution.status
                    <<" disk="<<snapshot.disk_io_status<<" lag_ms="<<snapshot.cpu_execution_lag_ms<<'\n';
                if(sample>3 && snapshot.cpu_execution_status!="ready" && snapshot.cpu_execution_status!="partial" &&
                    snapshot.cpu_execution_status!="baseline" && snapshot.cpu_execution_status!="initializing" &&
                    snapshot.cpu_execution_status!="awaiting-window")
                    throw std::runtime_error("CPU ETW trace failed: "+snapshot.cpu_execution_status);
                need(row.cpu_percent==42 && row.auto_scores[0]==7 && snapshot.total_cpu_percent==17,"collector must not alter CPU or AutoScore");
                for(const auto& cpu:row.cpu_execution.logical) {
                    if(cpu.percent)need(*cpu.percent>=0 && *cpu.percent<=100,"per-logical percentage range");
                    if(cpu.group==0 && cpu.runtime_ns>1000000) {
                        ++seen;if(cpu.number==a)++seen_a;if(cpu.number==b)++seen_b;
                    }
                }
                const auto& execution=row.cpu_execution;
                if(execution.frequency && execution.end_qpc>execution.begin_qpc) {
                    need(execution.begin_qpc>=last_end,"ETW measurement intervals must never overlap");last_end=execution.end_qpc;
                    std::uint64_t total_ns{};
                    for(const auto& cpu:execution.logical)total_ns+=cpu.runtime_ns;
                    const double interval=static_cast<double>(execution.end_qpc-execution.begin_qpc)/static_cast<double>(execution.frequency);
                    const double usage=100.0*static_cast<double>(total_ns)/(interval*1000000000.0);
                    observed_ns+=total_ns;
                    evidence<<"observed_cpu_percent="<<usage<<" interval_seconds="<<interval
                        <<" actual_cpu_ns="<<process_runtime_ns()-actual_start<<'\n';
                    // This PID includes the two tracing consumers, disk writer
                    // and main thread, not just the two pinned workers. Short
                    // delivery windows can legitimately exceed 250 percent.
                    // The independent process clock bounds cumulative runtime;
                    // it detects double accounting without assuming idle readers.
                    need(static_cast<double>(observed_ns)<=1.1*static_cast<double>(process_runtime_ns()-actual_start)+250'000'000.0,
                        "ETW time must not exceed independent cumulative process CPU time");
                    peak_cpu=std::max(peak_cpu,usage);
                }
                disk=disk || row.disk_write_bytes_per_second.value_or(0)>0;
            }
            worker.request_stop();migrating.request_stop();writer.request_stop();
            for(auto& thread:churn)thread.request_stop();
            need(work.load()!=0 && seen>0 && seen_a>0 && seen_b>0,"real pinned/migrating worker CPU attribution required");
            const auto actual_ns=process_runtime_ns()-actual_start;
            evidence<<"observed_ns="<<observed_ns<<" actual_ns="<<actual_ns<<" peak_cpu="<<peak_cpu<<std::endl;
            need(actual_ns>0 && static_cast<double>(observed_ns)>=0.3*static_cast<double>(actual_ns) &&
                static_cast<double>(observed_ns)<=1.1*static_cast<double>(actual_ns),
                "ETW CPU time must agree with the independent process clock, allowing initial/delivery tails");
            need(disk_bytes.load()>0 && disk,"physical ETW disk attribution must continue during CSwitch load");
        }
        need(trace_count()==initial,"normal shutdown must release the shared kernel session and both consumers");
        crash_and_restart(argv[1]);
        need(trace_count()==initial,"crash/restart fixture leaves no owned sessions behind");
        std::cout<<"Live CPU/disk coexistence, affinity, migration, shutdown and crash/restart passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
