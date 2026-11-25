#pragma once

#include "fluxtop/cpu_topology.hpp"
#include <array>

namespace fluxtop {
// Windows topology plus the CPUID vendor. This code needs no temperature driver.
std::array<char,12> read_cpu_vendor() noexcept;
CpuTopologySnapshot read_cpu_topology();
} // namespace fluxtop
