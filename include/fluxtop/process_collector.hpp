#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/nt_process_snapshot.hpp"

namespace fluxtop {
class ProcessCollector {
public:
    Snapshot collect();
private:
    CpuTracker tracker_;
    std::uint64_t idle_{}, kernel_{}, user_{};
    bool system_baseline_{};
    struct Path { std::uint64_t creation; std::string value; };
    std::unordered_map<std::uint32_t, Path> paths_;
    NtProcessSnapshot native_;
    bool native_unavailable_{};
};
} // namespace fluxtop
