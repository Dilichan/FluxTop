#pragma once
#include <cstdint>
#include <optional>
#include <cmath>
#include <limits>

namespace fluxtop {
// Converts the Windows GPU Process Memory / Local Usage counter into bytes.
// A valid zero stays zero, while impossible/nonfinite values stay unknown.
// Capacity, when known, refers to the sum of local physical-adapter capacity.
inline std::optional<std::uint64_t> gpu_process_memory_from_pdh(double bytes,
    std::optional<std::uint64_t> capacity) noexcept {
    if (!std::isfinite(bytes) || bytes < 0.0 ||
        bytes >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ||
        (capacity && *capacity && bytes > static_cast<double>(*capacity)))
        return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}
// D3DKMT allocation counts are not necessarily physically resident bytes.
// Reject values that contradict available adapter-wide usage or capacity;
// preserve unknown when neither adapter limit is reported.
inline bool gpu_process_memory_consistent(std::uint64_t process_bytes,
    std::optional<std::uint64_t> adapter_used,
    std::optional<std::uint64_t> dedicated_capacity) noexcept {
    return (!adapter_used || process_bytes <= *adapter_used) &&
        (!dedicated_capacity || !*dedicated_capacity || process_bytes <= *dedicated_capacity);
}
} // namespace fluxtop
