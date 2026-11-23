#include "fluxtop/sampler.hpp"
#include "fluxtop/temperatures.hpp"
#include "fluxtop/system_graph_history.hpp"
#include "fluxtop/output.hpp"
#include "fluxtop/windows.hpp"
#include <psapi.h>
#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <new>
struct Counts {std::uint64_t calls{},bytes{},frees{};};
thread_local Counts local_counts;
std::atomic<std::uint64_t> allocations{},allocation_bytes{},deallocations{};
void* allocate(std::size_t size,std::size_t alignment=0){void* p=alignment?_aligned_malloc(size?size:1,alignment):std::malloc(size?size:1);if(!p)throw std::bad_alloc();++local_counts.calls;local_counts.bytes+=size;++allocations;allocation_bytes+=size;return p;}
void release(void* p,bool aligned=false)noexcept{if(p){++local_counts.frees;++deallocations;if(aligned)_aligned_free(p);else std::free(p);}}
void* operator new(std::size_t n){return allocate(n);}void* operator new[](std::size_t n){return allocate(n);}
void operator delete(void* p)noexcept{release(p);}void operator delete[](void* p)noexcept{release(p);}
void operator delete(void* p,std::size_t)noexcept{release(p);}void operator delete[](void* p,std::size_t)noexcept{release(p);}
void* operator new(std::size_t n,std::align_val_t a){return allocate(n,static_cast<std::size_t>(a));}void* operator new[](std::size_t n,std::align_val_t a){return allocate(n,static_cast<std::size_t>(a));}
void operator delete(void* p,std::align_val_t)noexcept{release(p,true);}void operator delete[](void* p,std::align_val_t)noexcept{release(p,true);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{release(p,true);}void operator delete[](void* p,std::size_t,std::align_val_t)noexcept{release(p,true);}
std::uint64_t cpu(HANDLE handle,bool thread=false){FILETIME a{},b{},k{},u{};if(!(thread?GetThreadTimes(handle,&a,&b,&k,&u):GetProcessTimes(handle,&a,&b,&k,&u)))throw std::runtime_error("CPU clock");return fluxtop::filetime_ticks(k)+fluxtop::filetime_ticks(u);}
void write_process_api(std::ostream&,unsigned);
using Clock=std::chrono::steady_clock;
int wmain(int argc,wchar_t** argv){try{
 if(argc!=5)throw std::runtime_error("profile output.csv samples interval warmup");
 if(!fluxtop::initialize_privileges().elevated.value_or(false))return 740;
 const unsigned samples=static_cast<unsigned>(_wtoi(argv[2])),interval=static_cast<unsigned>(_wtoi(argv[3])),warm=static_cast<unsigned>(_wtoi(argv[4]));
 std::ofstream out(argv[1]);out<<"sample,stage,wall_ms,thread_cpu_ms,cpp_new,cpp_new_bytes,cpp_delete,process_cpu_ms,working_set,private_bytes,handles,processes,global_new,global_new_bytes,global_delete\n";
 fluxtop::ProcessCollector processes;fluxtop::GpuCollector gpu(true);fluxtop::NetworkCollector network(true);fluxtop::DiskCollector disks;fluxtop::DiskIoCollector io;fluxtop::CpuLogicalCollector logical;fluxtop::FpsCollector fps;fluxtop::SpikeTracker spikes;fluxtop::HistoryWriter history(fluxtop::history_path());fluxtop::SystemGraphHistory graphs;
 auto previous=Clock::now();auto last_cpu=cpu(GetCurrentProcess());
 for(unsigned n=0;n<samples+warm;++n){auto deadline=previous+std::chrono::milliseconds(interval);std::this_thread::sleep_until(deadline);previous=Clock::now();
  fluxtop::Snapshot s;auto measure=[&](const char* name,auto&& fn){auto before=local_counts;auto c=cpu(GetCurrentThread(),true);auto begin=Clock::now();fn();auto duration=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();auto ticks=cpu(GetCurrentThread(),true)-c;auto after=local_counts;
   if(n>=warm)out<<n-warm<<','<<name<<','<<duration<<','<<ticks/10000.0<<','<<after.calls-before.calls<<','<<after.bytes-before.bytes<<','<<after.frees-before.frees<<",0,0,0,0,0,0,0,0\n";};
  measure("process",[&]{s=processes.collect();});if(n>=warm)write_process_api(out,n-warm);else{std::ostringstream discarded;write_process_api(discarded,n);}measure("gpu",[&]{gpu.collect(s);});measure("fps",[&]{fps.collect(s.processes,{GetCurrentProcessId()});});
  measure("temperature",[&]{auto t=fluxtop::read_hardware_temperatures();s.cpu_temperature_c=t.cpu_c;s.intel_core_temperatures=std::move(t.intel_cores);});
  measure("logical",[&]{logical.collect(s.cpu_logical_usage,s.cpu_logical_status,s.intel_core_temperatures.logical_members);});
  measure("network",[&]{network.collect(s);});measure("disk_pdh",[&]{disks.collect(s);});measure("disk_etw",[&]{io.collect(s);});
  measure("autoscore",[&]{spikes.update(s.processes,s.total_ram_capacity,s.total_gpu_memory_capacity);});
  s.timestamp_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();s.interval_ms=interval;
  measure("history",[&]{history.append(s);});measure("group",[&]{auto grouped=fluxtop::aggregate_application_processes(s.processes);});
  measure("graphs",[&]{graphs.append(s);});
  PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));DWORD handles{};GetProcessHandleCount(GetCurrentProcess(),&handles);
  const auto now_cpu=cpu(GetCurrentProcess());if(n>=warm)out<<n-warm<<",cycle,"<<std::chrono::duration<double,std::milli>(Clock::now()-previous).count()<<",0,0,0,0,"<<(now_cpu-last_cpu)/10000.0<<','<<memory.WorkingSetSize<<','<<memory.PrivateUsage<<','<<handles<<','<<s.processes.size()<<','<<allocations.load()<<','<<allocation_bytes.load()<<','<<deallocations.load()<<'\n';last_cpu=now_cpu;
 }
 out.flush();return out?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
