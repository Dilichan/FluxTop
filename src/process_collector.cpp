#include "fluxtop/process_collector.hpp"
#include "fluxtop/windows.hpp"

#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>

namespace fluxtop {
Snapshot ProcessCollector::collect() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) throw windows_error("CreateToolhelp32Snapshot");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) throw windows_error("Process32FirstW");
    std::vector<RawProcess> raw;
    raw.reserve(512);
    Snapshot result;
    result.self_pid = GetCurrentProcessId();
    result.logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!result.logical_processors) throw windows_error("GetActiveProcessorCount");
    do {
        if (entry.th32ProcessID == 0) continue; // Idle is not an application.
        RawProcess row;
        row.pid = entry.th32ProcessID;
        row.name = utf8(entry.szExeFile);
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid));
        if (process.valid()) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                row.sampled_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                row.creation_time = filetime_ticks(created);
                row.cpu_ticks = filetime_ticks(kernel) + filetime_ticks(user);
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
    } while (Process32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw windows_error("Process32NextW");
    result.processes = tracker_.update(raw, result.logical_processors);
    for (const auto& row : result.processes) {
        if (row.pid == result.self_pid) result.self_cpu_percent = row.cpu_percent;
    }
    return result;
}
} // namespace fluxtop
