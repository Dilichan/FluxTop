#pragma once
#include <optional>
namespace fluxtop {
struct HardwareTemperatures {
    std::optional<double> cpu_c, gpu_c;
};
HardwareTemperatures read_hwinfo_temperatures();
} // namespace fluxtop