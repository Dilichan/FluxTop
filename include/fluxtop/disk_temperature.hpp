#pragma once
#include <array>
#include <optional>
namespace fluxtop {
// Snapshot of temperature of mounted drive letters; unknown is nullopt.
std::array<std::optional<double>,26> latest_disk_temperatures();
}