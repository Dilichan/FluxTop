#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fluxtop {
// Stable temperature identity. A CPU socket is not a CPU die measurement.
enum class CpuTemperatureKind : unsigned char { package, die, core_average, core, board, acpi };
enum class CpuTemperatureProvider : unsigned char { intel_dts, hwinfo, libre_wmi, open_wmi, acpi };
struct CpuTemperatureCandidate {
    CpuTemperatureProvider provider{CpuTemperatureProvider::intel_dts};
    CpuTemperatureKind kind{CpuTemperatureKind::package};
    std::optional<double> celsius;
    std::uint64_t sampled_tick{};
};
struct CpuTemperatureSelection {
    std::optional<double> celsius;
    CpuTemperatureProvider provider{CpuTemperatureProvider::intel_dts};
    CpuTemperatureKind kind{CpuTemperatureKind::package};
    std::uint64_t sampled_tick{};
};
// Pure, bounded, testable choice. Missing values and expired samples never win.
int cpu_temperature_rank(CpuTemperatureProvider provider,CpuTemperatureKind kind) noexcept;
bool cpu_temperature_candidate_valid(const CpuTemperatureCandidate& sample,
                                     std::uint64_t now,std::uint64_t ttl_ms=20000) noexcept;
class CpuTemperatureSelector {
public:
    CpuTemperatureSelection select(const CpuTemperatureCandidate* samples,std::size_t count,
                                   std::uint64_t now) noexcept;
private:
    std::optional<CpuTemperatureProvider> active_provider_;
    CpuTemperatureKind active_kind_{CpuTemperatureKind::package};
    std::optional<CpuTemperatureProvider> pending_provider_;
    CpuTemperatureKind pending_kind_{CpuTemperatureKind::package};
    unsigned pending_count_{};
};
std::string_view cpu_temperature_kind_name(CpuTemperatureKind kind) noexcept;
} // namespace fluxtop