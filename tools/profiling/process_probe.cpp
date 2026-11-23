#include "fluxtop/windows.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <ostream>
namespace {
struct Probe {const char* name;double ms{};unsigned calls{};};
Probe probes[]={{"toolhelp"},{"open"},{"times"},{"memory"},{"io"},{"image"},{"normalize"},{"next"}};
template<class F> auto measure(unsigned index,F&& f){auto start=std::chrono::steady_clock::now();auto value=f();const auto error=GetLastError();probes[index].ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++probes[index].calls;SetLastError(error);return value;}
HANDLE profile_snapshot(DWORD flags,DWORD pid){return measure(0,[&]{return CreateToolhelp32Snapshot(flags,pid);});}
HANDLE profile_open(DWORD access,BOOL inherit,DWORD pid){return measure(1,[&]{return OpenProcess(access,inherit,pid);});}
BOOL profile_times(HANDLE h,LPFILETIME a,LPFILETIME b,LPFILETIME c,LPFILETIME d){return measure(2,[&]{return GetProcessTimes(h,a,b,c,d);});}
BOOL profile_memory(HANDLE h,PPROCESS_MEMORY_COUNTERS p,DWORD n){return measure(3,[&]{return GetProcessMemoryInfo(h,p,n);});}
BOOL profile_io(HANDLE h,PIO_COUNTERS p){return measure(4,[&]{return GetProcessIoCounters(h,p);});}
BOOL profile_image(HANDLE h,DWORD f,LPWSTR p,PDWORD n){return measure(5,[&]{return QueryFullProcessImageNameW(h,f,p,n);});}
BOOL profile_next(HANDLE h,LPPROCESSENTRY32W p){return measure(7,[&]{return Process32NextW(h,p);});}
}
void write_process_api(std::ostream& out,unsigned sample){for(auto& p:probes){out<<sample<<",api."<<p.name<<','<<p.ms<<",0,"<<p.calls<<",0,0,0,0,0,0,0,0,0,0\n";p.ms=0;p.calls=0;}}
#define CreateToolhelp32Snapshot profile_snapshot
#define OpenProcess profile_open
#define GetProcessTimes profile_times
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo profile_memory
#define GetProcessIoCounters profile_io
#define QueryFullProcessImageNameW profile_image
#define Process32NextW profile_next
#include "../../src/process_collector.cpp"
