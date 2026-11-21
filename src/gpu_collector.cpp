#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/gpu_process_memory.hpp"
#include "fluxtop/diagnostics.hpp"
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
// Aggregated counts only; no PIDs or executable names are logged.
struct GpuAccessProbe {
    std::uint32_t attempted{}, opened_full{}, open_denied{}, open_other{};
    std::uint32_t identity_failed{}, query_full_ok{}, query_full_failed{};
    std::uint32_t returned{};
    LONG first_d3dkmt_failure{};
    bool has_d3dkmt_failure{};
};
bool collect_memory(Snapshot& snapshot, GpuAccessProbe& probe) {
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
        ++probe.attempted;
        // The observed WDDM D3DKMT path rejects limited-information handles
        // with STATUS_ACCESS_DENIED. Request the necessary right directly;
        // it does not, by itself, require an elevated process.
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,row.pid));
        if (!process.valid()) {
            if (GetLastError()==ERROR_ACCESS_DENIED) ++probe.open_denied;
            else ++probe.open_other;
            continue;
        }
        ++probe.opened_full;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.get(),&created,&exited,&kernel,&user) ||
            filetime_ticks(created)!=*row.creation_time) {
            ++probe.identity_failed;
            continue;
        }
        // Query every physical adapter with the same verified process handle.
        // If even one query fails, a partial per-PID sum is UNKNOWN, not zero.
        const auto query_usage = [&](HANDLE handle)->std::optional<std::uint64_t> {
            std::uint64_t total = 0;
            for (ULONG i=0; i<adapters.data.NumAdapters; ++i) {
                for (UINT node=0; node<physical_counts[i]; ++node) {
                    D3DKMT_QUERYVIDEOMEMORYINFO info{};
                    info.hProcess = handle;
                    info.hAdapter = adapters.data.Adapters[i].hAdapter;
                    info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
                    info.PhysicalAdapterIndex = node;
                    const NTSTATUS status=D3DKMTQueryVideoMemoryInfo(&info);
                    if(status < 0) {
                        if(!probe.has_d3dkmt_failure) {
                            probe.first_d3dkmt_failure=status;
                            probe.has_d3dkmt_failure=true;
                        }
                        return std::nullopt;
                    }
                    if(info.CurrentUsage > std::numeric_limits<std::uint64_t>::max()-total)
                        return std::nullopt;
                    total += info.CurrentUsage;
                }
            }
            return total;
        };
        const auto usage = query_usage(process.get());
        if (usage) {
            ++probe.query_full_ok;
            row.dedicated_gpu_bytes=*usage;
            ++probe.returned;
            available=true;
        } else {
            ++probe.query_full_failed;
        }
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
    GpuAccessProbe probe{};
    const bool memory_ok = collect_memory(snapshot,probe);
    // Diagnostic aggregation is deliberately bounded; no per-process log entries.
    if (probe.attempted>0 && (++gpu_access_samples_==1 || gpu_access_samples_%15==0)) {
        diagnostics::write(diagnostics::Level::info,"gpu.memory.access",
            "attempted="+std::to_string(probe.attempted)+
            " full_open="+std::to_string(probe.opened_full)+
            " open_denied="+std::to_string(probe.open_denied)+
            " open_other="+std::to_string(probe.open_other)+
            " identity_failed="+std::to_string(probe.identity_failed)+
            " full_query_ok="+std::to_string(probe.query_full_ok)+
            " full_query_failed="+std::to_string(probe.query_full_failed)+
            " valid_processes="+std::to_string(probe.returned)+
            " first_d3dkmt_ntstatus="+(probe.has_d3dkmt_failure?
                std::to_string(probe.first_d3dkmt_failure):"none"));
    }
    snapshot.gpu_status = engine_ok && memory_ok ? "ready"
        : engine_ok || memory_ok ? "partial" : "unavailable";
    bool inconsistent_usage = false;
    for (auto& row : snapshot.processes) {
        // D3DKMT reports allocated local memory. If it exceeds a known global
        // adapter usage or physical capacity, do not mislabel it as resident VRAM.
        // Never cap or synthesize bytes; unknown is preferable to false precision.
        if (row.dedicated_gpu_bytes && !gpu_process_memory_consistent(
                *row.dedicated_gpu_bytes, snapshot.total_gpu_memory,
                snapshot.total_gpu_memory_capacity)) {
            row.dedicated_gpu_bytes.reset();
            inconsistent_usage = true;
        }
        if (!row.has_baseline) continue;
        if (const auto found = engine.find(row.pid); found != engine.end())
            row.gpu_percent = found->second;
    }
    if (inconsistent_usage) {
        snapshot.gpu_status = "partial";
        if (!inconsistency_reported_) {
            diagnostics::write(diagnostics::Level::warning,"gpu.memory.consistency",
                "D3DKMT process usage exceeds adapter-wide usage or capacity; unknown published");
            inconsistency_reported_ = true;
        }
    }
}
} // namespace fluxtop
