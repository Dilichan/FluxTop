#include "fluxtop/process_collector.hpp"
#include "fluxtop/windows.hpp"

#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <algorithm>
#include <utility>

namespace fluxtop {
Snapshot ProcessCollector::collect() {
    auto entries=enumerator_.collect(GetTickCount64());
    std::vector<RawProcess> raw;
    std::unordered_map<std::uint32_t, Path> next_paths;
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
    for(auto& entry:entries) {
        RawProcess row;
        row.pid = entry.pid;
        row.parent_pid = entry.parent_pid;
        row.name = std::move(entry.name);
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid));
        if (process.valid()) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                row.sampled_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                row.creation_time = filetime_ticks(created);
                row.cpu_ticks = filetime_ticks(kernel) + filetime_ticks(user);
                if (const auto found = paths_.find(row.pid); found != paths_.end() && found->second.creation == *row.creation_time)
                    row.exe_path = found->second.value;
                else {
                    // A typical executable path fits in 512 wchar_t. Only allocate
                    // a large buffer for the exceptional long-path process.
                    wchar_t short_path[512]{};
                    DWORD length = static_cast<DWORD>(512U);
                    std::wstring path;
                    if (QueryFullProcessImageNameW(process.get(), 0, short_path, &length)) {
                        path.assign(short_path, length);
                    } else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                        path.resize(32768);
                        length = static_cast<DWORD>(path.size());
                        if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
                            path.resize(length);
                        else path.clear();
                    }
                    if (!path.empty()) {
                        // Invariant case folding makes Windows paths stable application keys.
                        std::wstring folded(length, L'\0');
                        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), static_cast<int>(length),
                            folded.data(), static_cast<int>(length), nullptr, nullptr, 0)) path = std::move(folded);
                        row.exe_path = utf8(path.c_str());
                    }
                }
                if (row.exe_path) next_paths.emplace(row.pid, Path{*row.creation_time, *row.exe_path});
            }
            IO_COUNTERS io{};
            if (GetProcessIoCounters(process.get(), &io)) {
                row.io_read_bytes = io.ReadTransferCount;
                row.io_write_bytes = io.WriteTransferCount;
            }
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                row.working_set = memory.WorkingSetSize;
                row.private_bytes = memory.PrivateUsage;
            }
        }
        if (!row.cpu_ticks || !row.working_set) ++result.inaccessible;
        raw.push_back(std::move(row));
    }
    paths_ = std::move(next_paths);
    // Transfer process names and paths instead of copying every string again.
    result.processes = tracker_.update(std::move(raw), result.logical_processors);
    for (const auto& row : result.processes) {
        if (row.pid == result.self_pid) result.self_cpu_percent = row.cpu_percent;
    }
    return result;
}
} // namespace fluxtop
