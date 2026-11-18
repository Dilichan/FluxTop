#pragma once
#include "fluxtop/model.hpp"
#include <windows.h>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fluxtop {
// Poll optional PresentMon data only for visible application PIDs supplied by the UI.
// A minimized/background process is measurable if it continues presenting frames.
class FpsCollector {
public:
    FpsCollector() = default;
    ~FpsCollector();
    FpsCollector(const FpsCollector&) = delete;
    FpsCollector& operator=(const FpsCollector&) = delete;
    // Receives current processes and selected PIDs; writes only valid presented FPS readings.
    void collect(std::vector<Process>& processes, const std::vector<std::uint32_t>& targets) noexcept;
private:
    // C ABI from PresentMonAPI2/PresentMonAPI.h, API major 3.
    struct QueryElement {
        std::int32_t metric{12}; // PM_METRIC_PRESENTED_FPS
        std::int32_t stat{1};    // PM_STAT_AVG
        std::uint32_t device_id{}, array_index{};
        std::uint64_t offset{}, size{};
    };
    static_assert(sizeof(QueryElement) == 32);
    using OpenFn = int (__cdecl*)(void**);
    using CloseFn = int (__cdecl*)(void*);
    using StartFn = int (__cdecl*)(void*, std::uint32_t);
    using StopFn = int (__cdecl*)(void*, std::uint32_t);
    using RegisterFn = int (__cdecl*)(void*, void**, QueryElement*, std::uint64_t, double, double);
    using FreeQueryFn = int (__cdecl*)(void*);
    using PollFn = int (__cdecl*)(void*, std::uint32_t, std::uint8_t*, std::uint32_t*);
    bool connect() noexcept;
    void disconnect() noexcept;
    HMODULE library_{};
    void* session_{};
    void* query_{};
    OpenFn open_{};
    CloseFn close_{};
    StartFn start_{};
    StopFn stop_{};
    RegisterFn register_{};
    FreeQueryFn free_query_{};
    PollFn poll_{};
    QueryElement element_{};
    // PID reuse must not borrow the prior process's FPS.
    std::unordered_map<std::uint32_t, std::optional<std::uint64_t>> tracked_;
    std::unordered_map<std::uint32_t, ULONGLONG> retry_after_;
    ULONGLONG next_connect_attempt_{};
};
} // namespace fluxtop
