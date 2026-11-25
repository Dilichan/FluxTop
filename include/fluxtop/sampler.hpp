#pragma once

#include "fluxtop/collection_plan.hpp"
#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/process_collector.hpp"
#include "fluxtop/cpu_logical_collector.hpp"
#include "fluxtop/network_collector.hpp"
#include "fluxtop/disk_collector.hpp"
#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/history.hpp"
#include "fluxtop/fps_collector.hpp"
#include "fluxtop/temperature_selection.hpp"
#include <chrono>
#include <memory>

namespace fluxtop {
class Sampler {
public:
    explicit Sampler(CollectionPlan plan);
    Snapshot collect();
    void set_fps_targets(std::vector<std::uint32_t> targets);
private:
    Privileges privileges_;
    ProcessCollector processes_;
    CpuLogicalCollector cpu_logical_;
    std::unique_ptr<GpuCollector> gpu_;
    SpikeTracker spikes_;
    std::unique_ptr<NetworkCollector> network_;
    DiskCollector disks_;
    DiskIoCollector disk_io_;
    CpuTemperatureSelector cpu_temperature_;
    std::unique_ptr<FpsCollector> fps_;
    bool fps_enabled_{};
    std::vector<std::uint32_t> fps_targets_;
    HistoryWriter history_{history_path()};
    std::chrono::steady_clock::time_point origin_;
    std::chrono::steady_clock::time_point previous_;
    std::uint64_t coverage_samples_{}; // Throttle metric availability diagnostics.
};

} // namespace fluxtop
