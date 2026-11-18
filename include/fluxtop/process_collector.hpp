#pragma once
#include "fluxtop/model.hpp"

namespace fluxtop {
class ProcessCollector {
public:
    Snapshot collect();
private:
    CpuTracker tracker_;
};
} // namespace fluxtop
