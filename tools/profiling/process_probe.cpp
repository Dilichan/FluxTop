#include "fluxtop/windows.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <ostream>
namespace {
struct Probe {const char* name;double ms{};unsigned calls{};};
Probe probes[]={{"open"},{"times"},{"memory"},{"io"}};
template<class F> auto measure(unsigned index,F&& f){auto start=std::chrono::steady_clock::now();auto value=f();const auto error=GetLastError();probes[index].ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++probes[index].calls;SetLastError(error);return value;}
HANDLE profile_open(DWORD access,BOOL inherit,DWORD pid){return measure(0,[&]{return OpenProcess(access,inherit,pid);});}
BOOL profile_times(HANDLE h,LPFILETIME a,LPFILETIME b,LPFILETIME c,LPFILETIME d){return measure(1,[&]{return GetProcessTimes(h,a,b,c,d);});}
BOOL profile_memory(HANDLE h,PPROCESS_MEMORY_COUNTERS p,DWORD n){return measure(2,[&]{return GetProcessMemoryInfo(h,p,n);});}
BOOL profile_io(HANDLE h,PIO_COUNTERS p){return measure(3,[&]{return GetProcessIoCounters(h,p);});}
}
void write_process_api(std::ostream& out,unsigned sample){for(auto& p:probes){out<<sample<<",api."<<p.name<<','<<p.ms<<",0,"<<p.calls<<",0,0,0,0,0,0,0,0,0,0\n";p.ms=0;p.calls=0;}}
#define OpenProcess profile_open
#define GetProcessTimes profile_times
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo profile_memory
#define GetProcessIoCounters profile_io
#include "../../src/process_collector.cpp"
