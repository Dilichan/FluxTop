#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/gpu_process_memory.hpp"
#include <winternl.h>
#include <d3dkmthk.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace fluxtop {
namespace {
// Owned kernel handles, released even when collection exits early.
struct Adapters {
    D3DKMT_ENUMADAPTERS data{};
    ~Adapters() {
        for (ULONG i = 0; i < data.NumAdapters; ++i) {
            D3DKMT_CLOSEADAPTER close{data.Adapters[i].hAdapter};
            D3DKMTCloseAdapter(&close);
        }
    }
};
bool collect_memory(Snapshot& snapshot) {
    Adapters adapters;
    if (D3DKMTEnumAdapters(&adapters.data) < 0) { adapters.data.NumAdapters = 0; return false; }
    if (!adapters.data.NumAdapters) return false;
    std::vector<UINT> physical_counts;
    std::uint64_t capacity = 0;
    bool has_capacity = false;
    for (ULONG i = 0; i < adapters.data.NumAdapters; ++i) {
        D3DKMT_PHYSICAL_ADAPTER_COUNT count{};
        D3DKMT_QUERYADAPTERINFO info{};
        info.hAdapter = adapters.data.Adapters[i].hAdapter;
        info.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
        info.pPrivateDriverData = &count;
        info.PrivateDriverDataSize = sizeof(count);
        if (D3DKMTQueryAdapterInfo(&info) < 0 || !count.Count) return false;
        physical_counts.push_back(count.Count);
        D3DKMT_SEGMENTSIZEINFO sizes{};
        D3DKMT_QUERYADAPTERINFO size_query{};
        size_query.hAdapter = adapters.data.Adapters[i].hAdapter;
        size_query.Type = KMTQAITYPE_GETSEGMENTSIZE;
        size_query.pPrivateDriverData = &sizes;
        size_query.PrivateDriverDataSize = sizeof(sizes);
        if (D3DKMTQueryAdapterInfo(&size_query) >= 0 && sizes.DedicatedVideoMemorySize > 0
            && sizes.DedicatedVideoMemorySize <= std::numeric_limits<std::uint64_t>::max() - capacity) {
            capacity += sizes.DedicatedVideoMemorySize;
            has_capacity = true;
        }
        for (UINT physical = 0; physical < count.Count; ++physical) {
            D3DKMT_ADAPTER_PERFDATA perf{};
            perf.PhysicalAdapterIndex = physical;
            D3DKMT_QUERYADAPTERINFO perf_query{};
            perf_query.hAdapter = adapters.data.Adapters[i].hAdapter;
            perf_query.Type = KMTQAITYPE_ADAPTERPERFDATA;
            perf_query.pPrivateDriverData = &perf;
            perf_query.PrivateDriverDataSize = sizeof(perf);
            if (D3DKMTQueryAdapterInfo(&perf_query) >= 0 && perf.Temperature >= 150 && perf.Temperature <= 1150) {
                const auto degrees = static_cast<double>(perf.Temperature) / 10.0;
                if (!snapshot.gpu_temperature_c || degrees > *snapshot.gpu_temperature_c)
                    snapshot.gpu_temperature_c = degrees;
            }
        }
    }
    if (has_capacity) snapshot.total_gpu_memory_capacity = capacity;
    bool available = false;
    for (auto& row : snapshot.processes) {
        if (!row.has_baseline || !row.creation_time) continue;
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, row.pid));
        if (!process.valid()) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)
            || filetime_ticks(created) != *row.creation_time) continue;
        std::uint64_t total = 0;
        bool valid = true;
        for (ULONG i = 0; i < adapters.data.NumAdapters && valid; ++i) {
            for (UINT node = 0; node < physical_counts[i]; ++node) {
                D3DKMT_QUERYVIDEOMEMORYINFO info{};
                info.hProcess = process.get();
                info.hAdapter = adapters.data.Adapters[i].hAdapter;
                info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
                info.PhysicalAdapterIndex = node;
                if (D3DKMTQueryVideoMemoryInfo(&info) < 0
                    || info.CurrentUsage > std::numeric_limits<std::uint64_t>::max() - total) {
                    valid = false; break;
                }
                total += info.CurrentUsage;
            }
        }
        if (valid) { row.dedicated_gpu_bytes = total; available = true; }
    }
    return available;
}
} // namespace
GpuCollector::GpuCollector(bool enabled) : enabled_(enabled) {
    if (!enabled_) return;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        std::cerr << "GPU: cannot open PDH query; CPU/RAM collection continues.\n";
        return;
    }
    const auto engine_status = PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine_);
    if (engine_status != ERROR_SUCCESS) engine_ = nullptr;
    if (PdhAddEnglishCounterW(query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &adapter_memory_) != ERROR_SUCCESS)
        adapter_memory_ = nullptr;
    // Same vendor-neutral PDH provider as GPU Engine; fallback for inaccessible D3DKMT process handles.
    if (PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &process_memory_) != ERROR_SUCCESS)
        process_memory_ = nullptr;
    if (!engine_) {
        std::cerr << "GPU: PDH engine counter unavailable (status=" << engine_status
                  << "); missing metrics stay unknown.\n";
    }
}

GpuCollector::~GpuCollector() {
    if (query_) PdhCloseQuery(query_);
}

bool GpuCollector::read(PDH_HCOUNTER counter,
                        std::unordered_map<std::uint32_t, double>& values, std::optional<double>& total, bool adapter_memory) {
    if (!counter) return false;
    constexpr DWORD max_bytes = 16 * 1024 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD bytes = static_cast<DWORD>(buffer_.size() * sizeof(std::max_align_t));
        DWORD count = 0;
        auto* items = buffer_.empty() ? nullptr : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        const auto status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
        if (status == PDH_MORE_DATA) {
            if (!bytes || bytes > max_bytes) return false;
            buffer_.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            continue; // Instances may change between size and data calls.
        }
        if (status != ERROR_SUCCESS) return false;
        bool valid = false;
        double memory_sum = 0;
        std::unordered_map<std::wstring, double> engines;
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const double value = item.FmtValue.doubleValue;
            if (!item.szName || !std::isfinite(value) || value < 0) continue;
            if (adapter_memory) { memory_sum += value; valid = true; continue; }
            const auto pid = gpu_instance_pid(item.szName);
            if (!pid) continue;
            valid = true;
            values[*pid] = std::max(values[*pid], std::clamp(value, 0.0, 100.0));
            const std::wstring_view name(item.szName);
            const auto identity=name.find(L"_luid_");
            if(identity!=std::wstring_view::npos) engines[std::wstring(name.substr(identity))]+=value;
        }
        if (valid) {
            total = memory_sum;
            if (!adapter_memory) for (const auto& [name, load] : engines) {
                (void)name; total = std::max(*total, std::clamp(load, 0.0, 100.0));
            }
        }
        return valid;
    }
    return false;
}

// Read validated per-process dedicated GPU memory from all LUID/physical adapters.
// Does not synthesize total adapter usage or replace unknown memory with zero.
bool GpuCollector::read_process_memory(std::unordered_map<std::uint32_t,std::uint64_t>& values) {
    if (!process_memory_) return false;
    constexpr DWORD max_bytes = 16 * 1024 * 1024;
    for (int attempt=0; attempt<3; ++attempt) {
        DWORD bytes = static_cast<DWORD>(buffer_.size() * sizeof(std::max_align_t));
        DWORD count = 0;
        auto* items = buffer_.empty() ? nullptr : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        const auto status = PdhGetFormattedCounterArrayW(process_memory_,
            PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
        if (status == PDH_MORE_DATA) {
            if (!bytes || bytes > max_bytes) return false;
            buffer_.resize((bytes + sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
            continue;
        }
        if (status != ERROR_SUCCESS) return false;
        GpuMemoryCounterAccumulator accumulator;
        for (DWORD i=0; i<count; ++i) {
            const auto& item = items[i];
            if (!item.szName || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA
                && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) continue;
            accumulator.add(item.szName,item.FmtValue.doubleValue);
        }
        values = std::move(accumulator).take();
        return !values.empty();
    }
    return false;
}

void GpuCollector::collect(Snapshot& snapshot) {
    snapshot.gpu_status = enabled_ ? "unavailable" : "disabled";
    if (!enabled_) return;
    std::unordered_map<std::uint32_t, double> engine;
    const bool collected = query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS;
    const bool engine_ok = collected && read(engine_, engine, snapshot.total_gpu_percent);
    std::optional<double> memory_total;
    if (collected && read(adapter_memory_, engine, memory_total, true) && memory_total
        && *memory_total < static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
        snapshot.total_gpu_memory = static_cast<std::uint64_t>(*memory_total);
    std::unordered_map<std::uint32_t,std::uint64_t> pdh_memory;
    const bool pdh_memory_ok = collected && read_process_memory(pdh_memory);
    const bool memory_ok = collect_memory(snapshot);
    snapshot.gpu_status = engine_ok && memory_ok ? "ready"
        : engine_ok || memory_ok || pdh_memory_ok ? "partial" : "unavailable";
    for (auto& row : snapshot.processes) {
        // PDH has PID-only identity. Avoid joining a new or reused process on its first sample.
        if (!row.has_baseline) continue;
        if (const auto found = engine.find(row.pid); found != engine.end()) row.gpu_percent = found->second;
        // A zero from a non-working driver query is ambiguous. Use positive PDH
        // values only when the primary D3DKMT value is absent or zero.
        if ((!row.dedicated_gpu_bytes || *row.dedicated_gpu_bytes == 0) && row.creation_time) {
            const auto found = pdh_memory.find(row.pid);
            if (found != pdh_memory.end() && found->second > 0) {
                // PDH identifies processes by PID only. Verify the instance is still
                // the same process before joining a counter to a snapshot row.
                Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,row.pid));
                FILETIME created{}, exited{}, kernel{}, user{};
                if (process.valid() && GetProcessTimes(process.get(),&created,&exited,&kernel,&user)
                    && filetime_ticks(created) == *row.creation_time)
                    row.dedicated_gpu_bytes = found->second;
            }
        }
    }
}
} // namespace fluxtop
