#pragma once
#include "fluxtop/model.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fluxtop {
// Aggregate validated per-adapter PDH readings into a per-PID total.
// Returns true only for a well-formed, finite reading not counted previously.
class GpuMemoryCounterAccumulator {
public:
    bool add(std::wstring_view instance, double used_bytes) {
        if (!std::isfinite(used_bytes) || used_bytes < 0.0 || used_bytes >= std::ldexp(1.0, 64))
            return false;
        std::wstring key(instance);
        const auto pid = gpu_instance_pid(key);
        if (!pid || seen_.contains(key)) return false;
        const auto bytes = static_cast<std::uint64_t>(used_bytes);
        auto& total = totals_[*pid];
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total) return false;
        total += bytes;
        seen_.insert(std::move(key));
        return true;
    }
    std::unordered_map<std::uint32_t,std::uint64_t> take() && {
        return std::move(totals_);
    }
private:
    std::unordered_set<std::wstring> seen_;
    std::unordered_map<std::uint32_t,std::uint64_t> totals_;
};
} // namespace fluxtop
