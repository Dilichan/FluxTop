#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace fluxtop {
struct IntelCoreTemperature {
    unsigned group{}, logical{}, core_id{};
    bool performance{};
    std::optional<double> celsius;
};
struct IntelDtsSnapshot {
    std::optional<double> package_c;
    std::vector<IntelCoreTemperature> cores;
    std::uint64_t sampled_tick{};
    bool driver_ready{};
};
// Pure MSR decoding; unknown values are not replaced with zero.
std::optional<double> intel_dts_decode(std::uint64_t target,std::uint64_t status) noexcept;
// Cached asynchronous reading; returns an empty measurement until first successful poll.
IntelDtsSnapshot read_intel_dts() noexcept;
} // namespace fluxtop