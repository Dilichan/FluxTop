#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fluxtop {
struct CpuCoreTemperature {
    unsigned group{}, logical{}, core_id{};
    bool performance{};
    std::optional<double> celsius;
    unsigned logical_siblings{1};
    // Some E-core DTS readings share a hardware sensor.
    bool sensor_independent_unverified{};
    bool amd{};
    bool operator==(const CpuCoreTemperature&)const=default;
};
struct LogicalCpuLocation {
    unsigned group{}, number{}, core_id{};
    bool performance{};
};
struct CpuTopologySnapshot {
    std::wstring cpu_name;
    std::optional<double> package_c;
    std::vector<CpuCoreTemperature> cores;
    std::vector<LogicalCpuLocation> logical_members; // One record per group-local logical CPU.
    std::uint64_t sampled_tick{};
    bool driver_ready{};
    unsigned physical_cores{}, logical_processors{}, available_cores{};
    std::optional<double> core_max_c, core_average_c;
    bool partial{};
    std::string status{"initializing"};
};
void finalize_core_temperatures(CpuTopologySnapshot& sample) noexcept;
} // namespace fluxtop
