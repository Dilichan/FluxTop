#pragma once
#include <cstdint>
#include <optional>

namespace fluxtop {
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
