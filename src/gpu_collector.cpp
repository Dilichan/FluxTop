#include "fluxtop/gpu_collector.hpp"
#include <pdhmsg.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace fluxtop {
GpuCollector::GpuCollector(bool enabled) : enabled_(enabled) {
    if (!enabled_) return;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        std::cerr << "GPU: cannot open PDH query; CPU/RAM collection continues.\n";
        return;
    }
    const auto engine_status = PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine_);
    const auto memory_status = PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory_);
    if (engine_status != ERROR_SUCCESS) engine_ = nullptr;
    if (memory_status != ERROR_SUCCESS) memory_ = nullptr;
    if (!engine_ || !memory_) {
        std::cerr << "GPU: some PDH counters are unavailable (engine=" << engine_status
                  << ", memory=" << memory_status << "); missing metrics stay unknown.\n";
    }
}

GpuCollector::~GpuCollector() {
    if (query_) PdhCloseQuery(query_);
}

bool GpuCollector::read(PDH_HCOUNTER counter, bool memory,
                        std::unordered_map<std::uint32_t, double>& values) {
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
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const double value = item.FmtValue.doubleValue;
            if (!item.szName || !std::isfinite(value) || value < 0) continue;
            const auto pid = gpu_instance_pid(item.szName);
            if (!pid) continue;
            valid = true;
            if (memory) values[*pid] += value;
            else values[*pid] = std::max(values[*pid], std::clamp(value, 0.0, 100.0));
        }
        return valid;
    }
    return false;
}

void GpuCollector::collect(Snapshot& snapshot) {
    snapshot.gpu_status = enabled_ ? "unavailable" : "disabled";
    if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
    std::unordered_map<std::uint32_t, double> engine, memory;
    const bool engine_ok = read(engine_, false, engine);
    const bool memory_ok = read(memory_, true, memory);
    snapshot.gpu_status = engine_ok && memory_ok ? "ready" : engine_ok || memory_ok ? "partial" : "unavailable";
    for (auto& row : snapshot.processes) {
        // PDH has PID-only identity. Avoid joining a new or reused process on its first sample.
        if (!row.has_baseline) continue;
        if (const auto found = engine.find(row.pid); found != engine.end()) row.gpu_percent = found->second;
        if (const auto found = memory.find(row.pid); found != memory.end()
            && found->second < static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
            row.dedicated_gpu_bytes = static_cast<std::uint64_t>(found->second);
    }
}
} // namespace fluxtop
