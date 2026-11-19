#pragma once
#include <windows.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <cstddef>

namespace fluxtop {
struct NtProcessRow {
    std::uint64_t created{};
    std::uint64_t cpu_ticks{};
    std::uint64_t working_set{};
    std::uint64_t private_bytes{};
};
class NtProcessSnapshot {
public:
    // Query the entire system in one call and validate layout against self.
    bool collect(std::unordered_map<std::uint32_t,NtProcessRow>& rows) noexcept;
private:
    std::vector<std::max_align_t> storage_;
};
} // namespace fluxtop
