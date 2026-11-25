#pragma once

#include "fluxtop/options.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fluxtop {

// UI-facing scheduler. Hardware providers are private to Sampler.
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
