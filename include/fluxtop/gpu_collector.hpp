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
    bool read(PDH_HCOUNTER counter, bool memory,
              std::unordered_map<std::uint32_t, double>& values);
    bool enabled_;
    PDH_HQUERY query_{};
    PDH_HCOUNTER engine_{};
    PDH_HCOUNTER memory_{};
    std::vector<std::max_align_t> buffer_;
};
} // namespace fluxtop
