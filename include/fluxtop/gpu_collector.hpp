#pragma once
#include "fluxtop/model.hpp"
#include <windows.h>
#include <pdh.h>
#include <cstddef>

namespace fluxtop {
class GpuCollector {
public:
    explicit GpuCollector(bool enabled);
    ~GpuCollector();
    GpuCollector(const GpuCollector&) = delete;
    GpuCollector& operator=(const GpuCollector&) = delete;
    void collect(Snapshot& snapshot);
private:
    bool read(PDH_HCOUNTER counter,
              std::unordered_map<std::uint32_t, double>& values, std::optional<double>& total,
              bool adapter_memory = false, bool process_memory = false);

    bool enabled_;
    PDH_HQUERY query_{};
    PDH_HCOUNTER engine_{};
    PDH_HCOUNTER adapter_memory_{};
    PDH_HCOUNTER process_memory_{};
    std::uint64_t gpu_access_samples_{};
    bool inconsistency_reported_{};
    std::vector<std::max_align_t> buffer_;
};
} // namespace fluxtop
