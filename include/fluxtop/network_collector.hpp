#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/etw_recovery.hpp"
#include <memory>

namespace fluxtop {
class NetworkCollector {
public:
    explicit NetworkCollector(bool enabled);
    ~NetworkCollector();
    void collect(Snapshot& snapshot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enabled_{};
    EtwRecovery recovery_;
};
} // namespace fluxtop
