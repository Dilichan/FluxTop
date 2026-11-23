#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace fluxtop {
struct IntelCoreTemperature {
    // v50-per-core-telemetry: topology identity is (processor group, affinity mask),
    // while core_id labels a physical core within the discovered topology.
    unsigned group{}, logical{}, core_id{};
    bool performance{};
    std::optional<double> celsius;
    unsigned logical_siblings{1};
    // E-core DTS may be shared or not individually addressable. No fabricated data.
    bool sensor_independent_unverified{};
    bool operator==(const IntelCoreTemperature&)const=default;
};
struct IntelLogicalProcessor {
    unsigned group{}, number{}, core_id{};
    bool performance{};
};
struct IntelDtsSnapshot {
    std::wstring cpu_name;
    std::optional<double> package_c;
    std::vector<IntelCoreTemperature> cores;
    std::vector<IntelLogicalProcessor> logical_members; // One record per logical CPU; group-aware.
    std::uint64_t sampled_tick{};
    bool driver_ready{};
    unsigned physical_cores{}, logical_processors{}, available_cores{};
    std::optional<double> core_max_c, core_average_c;
    bool partial{};
    std::string status{"initializing"}; // driver/missing-module/ready/partial, not a temperature.

};
// Detect physical/logical topology without PawnIO, UAC or MSR access.
IntelDtsSnapshot intel_dts_topology_snapshot();
// Clear expired/failed readings while retaining Windows topology.
void intel_dts_invalidate(IntelDtsSnapshot& sample) noexcept;
// Pure aggregation, also used by deterministic regression tests.
void intel_dts_finalize(IntelDtsSnapshot& sample) noexcept;
// Pure MSR decoding; unknown values are not replaced with zero.
std::optional<double> intel_dts_decode(std::uint64_t target,std::uint64_t status) noexcept;
// Cached asynchronous reading; returns an empty measurement until first successful poll.
IntelDtsSnapshot read_intel_dts() noexcept;
} // namespace fluxtop
