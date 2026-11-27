#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fluxtop {
// Stable temperature identity. A CPU socket is not a CPU die measurement.
enum class CpuTemperatureKind : unsigned char { package, die, core_average, core, board, acpi, control };
enum class CpuTemperatureProvider : unsigned char { intel_dts, hwinfo, libre_wmi, open_wmi, acpi, amd_smn };
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
// WMI can sleep while another source supplies a reading it cannot outrank.
bool cpu_temperature_needs_wmi(std::span<const CpuTemperatureCandidate> primary,
                               std::uint64_t now,std::uint64_t preferred_started_at=0) noexcept;
class CpuTemperatureSelector {
public:
    CpuTemperatureSelection select(std::span<const CpuTemperatureCandidate> samples,
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
