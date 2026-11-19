#include "fluxtop/nt_process_snapshot.hpp"
#include <psapi.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace fluxtop {
namespace {
// SYSTEM_PROCESS_INFORMATION native x64 prefix (Win10/11). Never assume this
// undocumented layout is valid: compile-time offsets and a live self-PID
// comparison must BOTH pass before its values enter any application snapshot.
struct NativeProcess {
    ULONG next;
    ULONG threads;
    LARGE_INTEGER private_working_set;
    ULONG hard_faults;
    ULONG max_threads;
    ULONGLONG cycles;
    LARGE_INTEGER created;
    LARGE_INTEGER user;
    LARGE_INTEGER kernel;
    struct { USHORT length, maximum; PWSTR buffer; } name;
    LONG base_priority;
    HANDLE pid;
    HANDLE parent;
    ULONG handles;
    ULONG session;
    ULONG_PTR key;
    SIZE_T peak_virtual;
    SIZE_T virtual_size;
    ULONG page_faults;
    SIZE_T peak_working;
    SIZE_T working;
    SIZE_T quota_peak_paged;
    SIZE_T quota_paged;
    SIZE_T quota_peak_nonpaged;
    SIZE_T quota_nonpaged;
    SIZE_T pagefile;
    SIZE_T peak_pagefile;
    SIZE_T private_pages;
};
static_assert(sizeof(void*)==8);
static_assert(offsetof(NativeProcess, created)==32);
static_assert(offsetof(NativeProcess, pid)==80);
static_assert(offsetof(NativeProcess, working)==144);
static_assert(offsetof(NativeProcess, private_pages)==200);
using Query = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG process_information = 5;
constexpr unsigned long info_length_mismatch = 0xc0000004UL;
std::uint64_t ticks(FILETIME time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime)<<32) | time.dwLowDateTime;
}
bool self_matches(const std::unordered_map<std::uint32_t,NtProcessRow>& rows) noexcept {
    const auto it=rows.find(GetCurrentProcessId());
    if(it==rows.end()) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX mem{}; mem.cb=sizeof(mem);
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) ||
       !GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),sizeof(mem)))
       return false;
    const auto& row=it->second;
    if(row.created!=ticks(created)) return false;
    const auto actual=ticks(kernel)+ticks(user);
    if(row.cpu_ticks>actual) {
        if(row.cpu_ticks-actual>10000000ULL) return false;
    } else if(actual-row.cpu_ticks>50000000ULL) return false;
    // A moving process can change its WS and private commit after the query.
    const auto near=[](std::uint64_t a, std::uint64_t b) noexcept {
        const auto diff=a>b?a-b:b-a;
        return diff<=std::max<std::uint64_t>(16ULL*1024*1024,b/4);
    };
    return near(row.working_set,mem.WorkingSetSize) && near(row.private_bytes,mem.PrivateUsage);
}
} // namespace

bool NtProcessSnapshot::collect(std::unordered_map<std::uint32_t,NtProcessRow>& rows) noexcept {
    rows.clear();
    const auto mod=GetModuleHandleW(L"ntdll.dll");
    if(!mod) return false;
    const auto addr=GetProcAddress(mod,"NtQuerySystemInformation");
    if(!addr) return false;
    Query query{};
    static_assert(sizeof(query)==sizeof(addr));
    std::memcpy(&query,&addr,sizeof(query));
    try {
        if(storage_.empty()) storage_.resize((256*1024)/sizeof(std::max_align_t));
        ULONG needed=0;
        LONG status=0;
        for(unsigned retry=0;retry<7;++retry) {
            const auto bytes=storage_.size()*sizeof(std::max_align_t);
            if(bytes>32ULL*1024*1024 || bytes>std::numeric_limits<ULONG>::max()) return false;
            status=query(process_information,storage_.data(),static_cast<ULONG>(bytes),&needed);
            if(status>=0) break;
            if(static_cast<unsigned long>(status)!=info_length_mismatch) return false;
            const auto grow=std::max<std::size_t>(bytes*2,static_cast<std::size_t>(needed)+65536);
            if(grow>32ULL*1024*1024) return false;
            storage_.resize((grow+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
        }
        if(status<0) return false;
        const auto capacity=storage_.size()*sizeof(std::max_align_t);
        const auto* data=reinterpret_cast<const std::byte*>(storage_.data());
        std::size_t offset=0;
        bool finished=false;
        for(unsigned n=0;n<100000;++n) {
            if(offset>capacity || capacity-offset<sizeof(NativeProcess)) return false;
            NativeProcess item{};
            std::memcpy(&item,data+offset,sizeof(item));
            const auto pid=reinterpret_cast<std::uintptr_t>(item.pid);
            if(pid>0 && pid<=std::numeric_limits<std::uint32_t>::max() &&
               item.created.QuadPart>0 && item.user.QuadPart>=0 && item.kernel.QuadPart>=0) {
                const auto cpu=static_cast<std::uint64_t>(item.user.QuadPart)+
                    static_cast<std::uint64_t>(item.kernel.QuadPart);
                rows.emplace(static_cast<std::uint32_t>(pid),NtProcessRow{
                    static_cast<std::uint64_t>(item.created.QuadPart),cpu,
                    static_cast<std::uint64_t>(item.working),static_cast<std::uint64_t>(item.private_pages)});
            }
            if(!item.next) { finished=true; break; }
            if(item.next<sizeof(NativeProcess) || item.next%8 || item.next>capacity-offset) return false;
            offset+=item.next;
        }
        if(!finished || !self_matches(rows)) {rows.clear();return false;}
        return true;
    } catch(...) {rows.clear();return false;}
}
} // namespace fluxtop
