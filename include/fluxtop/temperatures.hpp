#pragma once
#include <optional>
#include <string_view>
namespace fluxtop {
struct HardwareTemperatures {
    std::optional<double> cpu_c, gpu_c;
};
// The priority is based on CPU sensor identity, not motherboard vendor or model.
int cpu_temperature_sensor_priority(std::wstring_view identifier, std::wstring_view name) noexcept;
// HWiNFO shared memory; falls back to supported WMI hardware-monitor providers.
HardwareTemperatures read_hwinfo_temperatures();
HardwareTemperatures read_hardware_temperatures();
} // namespace fluxtop