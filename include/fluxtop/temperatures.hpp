#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace fluxtop {
struct HardwareTemperatures {
    std::optional<double> cpu_c, gpu_c;
    bool cpu_from_board{}; // Explicit CPU socket/board zone, NOT processor die/package.
    std::string cpu_source{"unavailable"}; // Diagnostic provenance, not a process metric.
    std::string hwinfo_status{"unavailable"}; // Last HWiNFO availability reason.
};
// Pure validation/selection rules: testable without HWiNFO or WMI installed.
bool cpu_temperature_valid(double celsius) noexcept;
bool cpu_temperature_fresh(std::int64_t polled_at, std::int64_t now, std::int64_t max_age) noexcept;
int hwinfo_cpu_sensor_priority(std::string_view group, std::string_view label) noexcept;
int hwinfo_board_sensor_priority(std::string_view group, std::string_view label) noexcept;
bool acpi_cpu_zone_candidate(std::wstring_view identifier) noexcept;
bool cpu_temperature_should_replace(bool current_known, bool current_board, bool incoming_board) noexcept;
// The priority is based on CPU sensor identity, not motherboard vendor or model.
int cpu_temperature_sensor_priority(std::wstring_view identifier, std::wstring_view name) noexcept;
int motherboard_cpu_sensor_priority(std::wstring_view identifier, std::wstring_view name) noexcept;
// HWiNFO shared memory; falls back to supported WMI hardware-monitor providers.
HardwareTemperatures read_hwinfo_temperatures();
HardwareTemperatures read_hardware_temperatures();
} // namespace fluxtop