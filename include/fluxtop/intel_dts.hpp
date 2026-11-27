#pragma once
#include "fluxtop/cpu_topology.hpp"
#include <string_view>
namespace fluxtop {
constexpr bool intel_dts_supports_vendor(std::string_view vendor) noexcept {
    return vendor=="GenuineIntel";
}
// Clear expired/failed readings while retaining Windows topology.
void intel_dts_invalidate(CpuTopologySnapshot& sample) noexcept;
// Pure aggregation, also used by deterministic regression tests.
// Pure MSR decoding; unknown values are not replaced with zero.
std::optional<double> intel_dts_decode(std::uint64_t target,std::uint64_t status) noexcept;
// Cached asynchronous reading; returns an empty measurement until first successful poll.
CpuTopologySnapshot read_intel_dts() noexcept;
} // namespace fluxtop
