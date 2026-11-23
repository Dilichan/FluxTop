#pragma once
#include <cmath>
#include <optional>

namespace fluxtop {
struct CpuLogicalUsage {
    unsigned group{}, number{}, core_id{};
    bool performance{};
    std::optional<double> percent; // null if no baseline, counter missing or stale.
    bool operator==(const CpuLogicalUsage&)const=default;
};
// Return null for invalid PDH data; never treat an unavailable measurement as zero.
inline std::optional<double> cpu_logical_percent(double percent) noexcept {
    if(!std::isfinite(percent) || percent < -0.01 || percent > 100.01) return std::nullopt;
    if(percent<0.0) percent=0.0;
    if(percent>100.0) percent=100.0;
    return percent;
}
} // namespace fluxtop
