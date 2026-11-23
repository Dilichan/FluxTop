#pragma once

#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/options.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/process_collector.hpp"
#include "fluxtop/cpu_logical_collector.hpp"
#include "fluxtop/network_collector.hpp"
#include "fluxtop/disk_collector.hpp"
#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/history.hpp"
#include "fluxtop/fps_collector.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fluxtop {
class Sampler {
public:
    Sampler(bool gpu, bool network, bool foreground_fps = false);
    Snapshot collect();
    void set_fps_targets(std::vector<std::uint32_t> targets);
private:
    Privileges privileges_;
    ProcessCollector processes_;
    CpuLogicalCollector cpu_logical_;
    GpuCollector gpu_;
    SpikeTracker spikes_;
    NetworkCollector network_;
    DiskCollector disks_;
    DiskIoCollector disk_io_;
    FpsCollector fps_;
    bool fps_enabled_{};
    std::vector<std::uint32_t> fps_targets_;
    HistoryWriter history_{history_path()};
    std::chrono::steady_clock::time_point origin_;
    std::chrono::steady_clock::time_point previous_;
    std::uint64_t coverage_samples_{}; // Throttle metric availability diagnostics.
};

class SamplingWorker {
public:
    using Notify = std::function<void(std::optional<Snapshot>, std::string, bool)>;
    ~SamplingWorker();
    void start(Options options, Notify notify);
    void pause(bool paused);
    void stop();
    void set_interval(unsigned milliseconds);
    void set_fps_targets(std::vector<std::uint32_t> targets);
private:
    std::atomic<bool> paused_{false};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread thread_;
    std::vector<std::uint32_t> fps_targets_;
    std::atomic<unsigned> interval_{1000}, revision_{0};
};
} // namespace fluxtop
