#pragma once
// Conservative, transport-agnostic ETW disk attribution primitives.
// SATA, NVMe, SAS and mechanical HDDs all expose physical disk offsets through ETW.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace fluxtop {
struct PhysicalVolumeExtent {
    std::uint32_t disk{};
    std::uint64_t offset{},length{};
    wchar_t letter{};
};
// Return a letter only if an operation belongs in precisely one entire volume extent.
// Multi-volume, cross-boundary and unknown operations remain unattributed to a letter.
inline std::optional<wchar_t> disk_io_volume_for(const std::vector<PhysicalVolumeExtent>& extents,
    std::uint32_t disk,std::uint64_t offset,std::uint64_t bytes) noexcept {
    if(!bytes || offset>UINT64_MAX-bytes)return std::nullopt;
    std::optional<wchar_t> found;
    for(const auto& extent:extents) {
        if(extent.disk!=disk || !extent.length || extent.offset>UINT64_MAX-extent.length)continue;
        if(offset>=extent.offset && offset+bytes<=extent.offset+extent.length) {
            if(found && *found!=extent.letter)return std::nullopt;
            found=extent.letter;
        }
    }
    return found;
}
// Require substantial attribution coverage before publishing a rate or an AutoScore.
inline bool disk_io_trustworthy(std::uint64_t matched,std::uint64_t total,bool lost_events) noexcept {
    return !lost_events && total>0 && matched<=total &&
        static_cast<double>(matched)/static_cast<double>(total)>=0.80;
}
// Heuristic: apportion measured volume active time by its fraction of *physical* bytes.
// Not a promise of the process's precise service time or a benchmark of the device.
inline std::optional<double> disk_io_pressure(std::optional<double> busy,
    std::uint64_t attributed_bytes,std::uint64_t volume_bytes) noexcept {
    if(!busy || !std::isfinite(*busy) || *busy<0 || volume_bytes==0 ||
        attributed_bytes>volume_bytes)return std::nullopt;
    return std::clamp(*busy,0.0,100.0)*
        static_cast<double>(attributed_bytes)/static_cast<double>(volume_bytes);
}
}
