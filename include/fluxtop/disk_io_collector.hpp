#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/etw_recovery.hpp"
#include <memory>

namespace fluxtop {
// Native ETW kernel disk/file correlation; fails closed on loss/ambiguity.
class DiskIoCollector {
public:
    DiskIoCollector();
    ~DiskIoCollector();
    DiskIoCollector(const DiskIoCollector&)=delete;
    DiskIoCollector& operator=(const DiskIoCollector&)=delete;
    void collect(Snapshot& snapshot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EtwRecovery recovery_;
};
}
