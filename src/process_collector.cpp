#include "fluxtop/process_collector.hpp"
#include "fluxtop/windows.hpp"

#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <algorithm>
#include <utility>

namespace fluxtop {
Snapshot ProcessCollector::collect() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) throw windows_error("CreateToolhelp32Snapshot");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) throw windows_error("Process32FirstW");
    std::vector<RawProcess> raw;
    std::unordered_map<std::uint32_t, Path> next_paths;
    std::unordered_map<std::uint32_t, NtProcessRow> native_rows;
    // Environment override exists for the AUTOMATED A/B test only.
    wchar_t legacy_mode[4]{};
    const bool legacy = GetEnvironmentVariableW(L"FLUXTOP_FORCE_LEGACY_PROCESSES",legacy_mode,4)>0 && legacy_mode[0]==L'1';
    const bool native_ready = !legacy && !native_unavailable_ && native_.collect(native_rows);
    if (!legacy && !native_ready) native_unavailable_ = true;
    const auto native_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    raw.reserve(512);
    next_paths.reserve(paths_.size());
    Snapshot result;
    FILETIME system_idle{}, system_kernel{}, system_user{};
    if (GetSystemTimes(&system_idle, &system_kernel, &system_user)) {
        const auto i = filetime_ticks(system_idle), k = filetime_ticks(system_kernel), u = filetime_ticks(system_user);
        if (system_baseline_ && i >= idle_ && k >= kernel_ && u >= user_) {
            const auto total = (k - kernel_) + (u - user_);
            if (total && i - idle_ <= total)
                result.total_cpu_percent = std::clamp(100.0 * static_cast<double>(total - (i - idle_)) / static_cast<double>(total), 0.0, 100.0);
        }
        idle_ = i; kernel_ = k; user_ = u; system_baseline_ = true;
    } else system_baseline_ = false;
    MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        result.total_ram_capacity = status.ullTotalPhys;
        result.total_ram_used = status.ullTotalPhys - status.ullAvailPhys;
    }
    result.self_pid = GetCurrentProcessId();
    result.logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!result.logical_processors) throw windows_error("GetActiveProcessorCount");
    do {
        if (entry.th32ProcessID == 0) continue; // Idle is not an application.
        RawProcess row;
        row.pid = entry.th32ProcessID;
        row.parent_pid = entry.th32ParentProcessID;
        row.name = utf8(entry.szExeFile);
        const auto fast = native_ready ? native_rows.find(row.pid) : native_rows.end();
        const bool have_native = native_ready && fast != native_rows.end();
        if (have_native) {
            row.creation_time = fast->second.created;
            row.cpu_ticks = fast->second.cpu_ticks;
            row.sampled_ns = native_ns;
            row.working_set = fast->second.working_set;
            row.private_bytes = fast->second.private_bytes;
        }
        const auto cached = row.creation_time ? paths_.find(row.pid) : paths_.end();
        const bool have_path = cached != paths_.end() && cached->second.creation == *row.creation_time;
        if (have_path) row.exe_path = cached->second.value;
        // Fast path: skip handles completely for unchanged known processes.
        // First-seen processes still need one handle to identify their image path.
        if (!have_native || !have_path) {
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid));
            if (process.valid()) {
                if (!have_native) {
                    FILETIME created{}, exited{}, kernel{}, user{};
                    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                        row.sampled_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        row.creation_time = filetime_ticks(created);
                        row.cpu_ticks = filetime_ticks(kernel) + filetime_ticks(user);
                    }
                    PROCESS_MEMORY_COUNTERS_EX memory{};
                    memory.cb = sizeof(memory);
                    if (GetProcessMemoryInfo(process.get(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) {
                        row.working_set = memory.WorkingSetSize;
                        row.private_bytes = memory.PrivateUsage;
                    }
                }
                if (row.creation_time) {
                    if (const auto found=paths_.find(row.pid);
                        found!=paths_.end() && found->second.creation==*row.creation_time)
                        row.exe_path=found->second.value;
                    else {
                        wchar_t short_path[512]{};
                        DWORD length=512U;
                        std::wstring path;
                        if(QueryFullProcessImageNameW(process.get(),0,short_path,&length)) path.assign(short_path,length);
                        else if(GetLastError()==ERROR_INSUFFICIENT_BUFFER) {
                            path.resize(32768);length=static_cast<DWORD>(path.size());
                            if(QueryFullProcessImageNameW(process.get(),0,path.data(),&length)) path.resize(length);
                            else path.clear();
                        }
                        if(!path.empty()) {
                            std::wstring folded(length,L'\0');
                            if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,path.data(),static_cast<int>(length),
                                folded.data(),static_cast<int>(length),nullptr,nullptr,0)) path=std::move(folded);
                            row.exe_path=utf8(path.c_str());
                        }
                    }
                }
            }
        }
        if (row.creation_time && row.exe_path)
            next_paths.emplace(row.pid,Path{*row.creation_time,*row.exe_path});
        if (!row.cpu_ticks || !row.working_set) ++result.inaccessible;
        raw.push_back(std::move(row));
    } while (Process32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw windows_error("Process32NextW");
    paths_ = std::move(next_paths);
    result.process_backend = native_ready ? "native" : "win32";
    // Transfer process names and paths instead of copying every string again.
    result.processes = tracker_.update(std::move(raw), result.logical_processors);
    for (const auto& row : result.processes) {
        if (row.pid == result.self_pid) result.self_cpu_percent = row.cpu_percent;
    }
    return result;
}
} // namespace fluxtop
