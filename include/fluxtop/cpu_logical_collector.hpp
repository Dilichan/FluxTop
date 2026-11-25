#pragma once
#include "fluxtop/cpu_logical.hpp"
#include "fluxtop/cpu_topology.hpp"
#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>

namespace fluxtop {
// PDH query owns all per-logical counters; no duplicate Windows CPU topology scanner.
class CpuLogicalCollector {
public:
    CpuLogicalCollector()=default;
    ~CpuLogicalCollector();
    CpuLogicalCollector(const CpuLogicalCollector&)=delete;
    CpuLogicalCollector& operator=(const CpuLogicalCollector&)=delete;
    void collect(std::vector<CpuLogicalUsage>& values,std::string& status,
        const std::vector<LogicalCpuLocation>& topology);
private:
    struct Counter {
        unsigned group{},number{},core_id{};
        bool performance{};
        PDH_HCOUNTER handle{};
    };
    void reset() noexcept;
    bool configure(const std::vector<LogicalCpuLocation>& topology);
    PDH_HQUERY query_{};
    std::vector<Counter> counters_;
    bool have_baseline_{};
    ULONGLONG previous_tick_{};
};
} // namespace fluxtop
