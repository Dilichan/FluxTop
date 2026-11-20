#pragma once
#include "fluxtop/model.hpp"
#include <windows.h>
#include <pdh.h>
#include <vector>

namespace fluxtop {
class DiskCollector {
public:
    DiskCollector() = default;
    ~DiskCollector();
    DiskCollector(const DiskCollector&) = delete;
    DiskCollector& operator=(const DiskCollector&) = delete;
    void collect(Snapshot& snapshot);
private:
    struct Counter { wchar_t letter; PDH_HCOUNTER handle{}; };
    void refresh(DWORD drive_mask);
    PDH_HQUERY query_{};
    DWORD last_mask_{};
    bool initialized_{};
    std::vector<Counter> counters_;
};
} // namespace fluxtop