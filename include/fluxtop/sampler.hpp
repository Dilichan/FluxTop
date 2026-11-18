#pragma once

#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/options.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/process_collector.hpp"
#include "fluxtop/network_collector.hpp"
#include "fluxtop/history.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fluxtop {
class Sampler {
public:
    Sampler(bool gpu, bool network);
    Snapshot collect();
private:
    Privileges privileges_;
    ProcessCollector processes_;
    GpuCollector gpu_;
    SpikeTracker spikes_;
    NetworkCollector network_;
    HistoryWriter history_{history_path()};
    std::chrono::steady_clock::time_point origin_;
    std::chrono::steady_clock::time_point previous_;
};

class SamplingWorker {
public:
    using Notify = std::function<void(std::optional<Snapshot>, std::string, bool)>;
    ~SamplingWorker();
    void start(Options options, Notify notify);
    void pause(bool paused);
    void stop();
    void set_interval(unsigned milliseconds);
private:
    std::atomic<bool> paused_{false};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread thread_;
    std::atomic<unsigned> interval_{1000}, revision_{0};
};
} // namespace fluxtop
