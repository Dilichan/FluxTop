#include "fluxtop/windows.hpp"
#include <winternl.h>
#include <tlhelp32.h>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
struct Entry {std::uint32_t parent{};std::wstring name;std::uint64_t generation{};};
using Entries=std::unordered_map<std::uint32_t,Entry>;
using Query=NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG);
Query query{};std::vector<std::byte> buffer(65536);
Entries basic(){ULONG bytes{};for(unsigned attempt=0;attempt<8;++attempt){const auto code=query(SystemBasicProcessInformation,buffer.data(),static_cast<ULONG>(buffer.size()),&bytes);
 if(code==static_cast<NTSTATUS>(0xc0000004u)){buffer.resize(std::max(buffer.size()*2,static_cast<std::size_t>(bytes)));continue;}
 if(code<0)throw std::runtime_error("Basic process information unsupported");
 Entries rows;std::size_t offset=0;while(true){if(offset+sizeof(SYSTEM_BASICPROCESS_INFORMATION)>buffer.size())throw std::runtime_error("record bounds");
 auto* p=reinterpret_cast<const SYSTEM_BASICPROCESS_INFORMATION*>(buffer.data()+offset);
 rows.emplace(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p->UniqueProcessId)),Entry{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p->InheritedFromUniqueProcessId)),std::wstring(p->ImageName.Buffer,p->ImageName.Length/2),p->SequenceNumber});
 if(!p->NextEntryOffset)break;offset+=p->NextEntryOffset;}return rows;}throw std::runtime_error("Enumeration never stabilized");}
Entries toolhelp(){fluxtop::Handle list(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(!list.valid())throw std::runtime_error("toolhelp");PROCESSENTRY32W p{};p.dwSize=sizeof(p);if(!Process32FirstW(list.get(),&p))throw std::runtime_error("toolhelp first");Entries rows;do{rows.emplace(p.th32ProcessID,Entry{p.th32ParentProcessID,p.szExeFile,0});}while(Process32NextW(list.get(),&p));return rows;}
int wmain(int argc,wchar_t** argv){try{if(argc!=2)return 2;query=std::bit_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));if(!query)return 77;std::ofstream out(argv[1]);out<<"sample,basic_ms,toolhelp_ms,matched,basic_count,toolhelp_count,buffer_bytes\n";using Clock=std::chrono::steady_clock;
 for(unsigned i=0;i<100;++i){auto start=Clock::now();auto a=basic();const auto btime=std::chrono::duration<double,std::milli>(Clock::now()-start).count();start=Clock::now();auto b=toolhelp();const auto ttime=std::chrono::duration<double,std::milli>(Clock::now()-start).count();const auto c=basic();unsigned matched=0;
 for(const auto& [pid,entry]:a){const auto old=b.find(pid);const auto now=c.find(pid);if(pid && old!=b.end() && now!=c.end() && entry.generation==now->second.generation){if(old->second.parent!=entry.parent || (entry.name.size()<MAX_PATH && old->second.name!=entry.name))throw std::runtime_error("Stable process identity/name/parent differs");++matched;}}
 if(matched<2)throw std::runtime_error("Too few matched processes");out<<i<<','<<btime<<','<<ttime<<','<<matched<<','<<a.size()<<','<<b.size()<<','<<buffer.size()<<'\n';Sleep(50);
 }return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
