#pragma once
#include "fluxtop/cpu_view.hpp"
#include <array>
#include <deque>
#include <memory>

namespace fluxtop {
struct FocusedFpsSource {
    std::uint64_t epoch{};
    std::uint32_t pid{};
    std::optional<std::uint64_t> creation_time;
    std::string name;
};
struct FocusedFpsPoint {
    double value{};
    std::shared_ptr<const FocusedFpsSource> source;
};
struct SystemChartPoint {
    std::int64_t time{};
    std::array<std::optional<double>,5> values{}; // CPU%, RAM%, Mbps, GPU%, VRAM%.
    std::array<std::optional<double>,26> disks{};
    std::optional<double> cpu_temperature;
    std::array<std::optional<double>,26> disk_temperatures{};
    CpuChartSample cpu;
    std::optional<FocusedFpsPoint> fps;
};
// Owned by the running monitor, not by a client window. Accessed only on the
// UI/message thread; no full process snapshots or icon resources are retained.
class SystemGraphHistory {
public:
    static constexpr std::int64_t retention_ms=30*60*1000;
    static constexpr std::size_t maximum_points=7201; // Full 30 minutes at 250 ms.
    bool append(const Snapshot& snapshot,std::uint64_t tick=0,
        std::optional<std::int64_t> wall_ms={});
    const std::deque<SystemChartPoint>& points()const{return points_;}
private:
    std::deque<SystemChartPoint> points_;
};
}
