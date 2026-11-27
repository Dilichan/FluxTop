#pragma once
#include "fluxtop/cpu_topology.hpp"
#include "fluxtop/temperature_selection.hpp"
#include <span>

namespace fluxtop {
struct AmdCoreLayout {
    CpuTopologySnapshot topology;
    std::vector<std::size_t> libre_core_order;
    std::uint64_t order_since{};
    bool single_package{};
};
struct AmdCoreReading {
    unsigned ordinal{};
    double celsius{};
};
bool amd_zen_family(unsigned family) noexcept;
std::optional<double> decode_amd_tctl(std::uint32_t raw) noexcept;
std::optional<unsigned> libre_amd_core_ordinal(std::wstring_view identifier,std::wstring_view name) noexcept;
std::vector<std::size_t> amd_core_order(std::span<const unsigned> hardware_ids);
void apply_amd_core_temperatures(CpuTopologySnapshot& topology,std::span<const std::size_t> order,
    std::span<const AmdCoreReading> readings,std::uint64_t sampled,std::uint64_t now);
bool amd_cpu() noexcept;
AmdCoreLayout read_amd_layout();
CpuTemperatureCandidate read_amd_tctl(bool single_package);
}
