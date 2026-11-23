#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/process_enumerator.hpp"

namespace fluxtop {
class ProcessCollector {
public:
    Snapshot collect();
private:
    CpuTracker tracker_;
    ProcessEnumerator enumerator_;
    std::uint64_t idle_{}, kernel_{}, user_{};
    bool system_baseline_{};
    struct Path { std::uint64_t creation; std::string value; };
    std::unordered_map<std::uint32_t, Path> paths_;
};
} // namespace fluxtop
