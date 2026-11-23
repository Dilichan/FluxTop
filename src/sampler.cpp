#include "fluxtop/sampler.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/temperatures.hpp"
#include <psapi.h>
#include <iostream>
#include <array>
#include <cmath>

namespace fluxtop {
Sampler::Sampler(bool gpu, bool network, bool foreground_fps) : privileges_(initialize_privileges()), gpu_(gpu), network_(network), fps_enabled_(foreground_fps), origin_(std::chrono::steady_clock::now()) {
    if (privileges_.elevated.value_or(false) && !privileges_.debug_enabled)
        std::cerr << "FluxTop: SeDebugPrivilege unavailable; collecting accessible process metrics.\n";
    auto baseline = processes_.collect();
    gpu_.collect(baseline);
    network_.collect(baseline);
    previous_ = std::chrono::steady_clock::now();
}

void Sampler::set_fps_targets(std::vector<std::uint32_t> targets) { fps_targets_ = std::move(targets); }

Snapshot Sampler::collect() {
    const auto start = std::chrono::steady_clock::now();
    auto snapshot = processes_.collect();
    snapshot.elevated = privileges_.elevated;
    snapshot.debug_privilege_enabled = privileges_.debug_enabled;
    gpu_.collect(snapshot);
    if (fps_enabled_) fps_.collect(snapshot.processes, fps_targets_);
    const auto hwtemps = read_hardware_temperatures();
    snapshot.cpu_temperature_c = hwtemps.cpu_c;
    snapshot.intel_core_temperatures = hwtemps.intel_cores;
    cpu_logical_.collect(snapshot.cpu_logical_usage,snapshot.cpu_logical_status,
        snapshot.intel_core_temperatures.logical_members);
    snapshot.cpu_temperature_board = hwtemps.cpu_from_board;
    snapshot.cpu_temperature_source = hwtemps.cpu_source;
    if (!snapshot.gpu_temperature_c) snapshot.gpu_temperature_c = hwtemps.gpu_c;
    network_.collect(snapshot);
    disks_.collect(snapshot);
    disk_io_.collect(snapshot); // Physical ETW attribution; never generic process I/O.
    // Observability only: count valid samples; never change or impute measurements.
    // A collector may be operational while many protected PIDs remain unknown.
    if (++coverage_samples_==1 || coverage_samples_%15==0) {
        std::array<std::size_t,8> known{}; // CPU, RAM, GPU, VRAM, Rx, Tx, disk read, disk write.
        for(const auto& row:snapshot.processes) {
            if(row.cpu_percent && std::isfinite(*row.cpu_percent) && *row.cpu_percent>=0.0)++known[0];
            if(row.working_set)++known[1];
            if(row.gpu_percent && std::isfinite(*row.gpu_percent) && *row.gpu_percent>=0.0)++known[2];
            if(row.dedicated_gpu_bytes)++known[3];
            if(row.rx_bytes_per_second && std::isfinite(*row.rx_bytes_per_second) && *row.rx_bytes_per_second>=0.0)++known[4];
            if(row.tx_bytes_per_second && std::isfinite(*row.tx_bytes_per_second) && *row.tx_bytes_per_second>=0.0)++known[5];
            if(row.disk_read_bytes_per_second)++known[6];
            if(row.disk_write_bytes_per_second)++known[7];
        }
        diagnostics::write(diagnostics::Level::info,"metrics.coverage",
            "processes="+std::to_string(snapshot.processes.size())+
            " elevated="+(privileges_.elevated.value_or(false)?"true":"false")+
            " cpu="+std::to_string(known[0])+" ram="+std::to_string(known[1])+
            " gpu="+std::to_string(known[2])+" vram="+std::to_string(known[3])+
            " rx="+std::to_string(known[4])+" tx="+std::to_string(known[5])+
            " disk_read="+std::to_string(known[6])+" disk_write="+std::to_string(known[7])+
            " gpu_status="+snapshot.gpu_status+
            " network_status="+snapshot.network_status+
            " disk_status="+snapshot.disk_io_status);
    }
    spikes_.update(snapshot.processes, snapshot.total_ram_capacity, snapshot.total_gpu_memory_capacity);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        snapshot.self_working_set = memory.WorkingSetSize;
        snapshot.self_private_bytes = memory.PrivateUsage;
        snapshot.self_peak_working_set = memory.PeakWorkingSetSize;
    }
    DWORD handles{};
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) snapshot.self_handles = handles;
    snapshot.collection_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    snapshot.elapsed_ms = std::chrono::duration<double, std::milli>(start - origin_).count();
    snapshot.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    snapshot.interval_ms = std::chrono::duration<double, std::milli>(start - previous_).count();
    previous_ = start;
    history_.append(snapshot);
    diagnostics::write(snapshot.history_error.empty()?diagnostics::Level::trace:diagnostics::Level::warning,
        "sampler.snapshot","processes="+std::to_string(snapshot.processes.size())+
        " collection_ms="+std::to_string(snapshot.collection_ms)+
        " history_ms="+std::to_string(snapshot.history_write_ms)+
        " history_error="+snapshot.history_error+" disk_io="+snapshot.disk_io_status);
    return snapshot;
}

SamplingWorker::~SamplingWorker() { stop(); }
void SamplingWorker::set_interval(unsigned milliseconds) {
    { std::lock_guard lock(mutex_); interval_ = milliseconds; ++revision_; }
    wake_.notify_all();
}
void SamplingWorker::set_fps_targets(std::vector<std::uint32_t> targets) {
    std::lock_guard lock(mutex_);
    fps_targets_ = std::move(targets);
}
void SamplingWorker::pause(bool paused) { paused_ = paused; wake_.notify_all(); }
void SamplingWorker::stop() {
    diagnostics::write(diagnostics::Level::info,"sampler.stop","sampling worker stop requested");
    if (thread_.joinable()) { thread_.request_stop(); wake_.notify_all(); thread_.join(); }
}
void SamplingWorker::start(Options options, Notify notify) {
    diagnostics::write(diagnostics::Level::info,"sampler.start","interval_ms="+std::to_string(options.interval_ms));
    interval_ = options.interval_ms;
    thread_ = std::jthread([this, options, notify = std::move(notify)](std::stop_token stop) {
        unsigned emitted = 0;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [this] { return !paused_.load(); });
            }
            if (stop.stop_requested()) return;
            try {
                Sampler sampler(options.gpu, options.network, options.overlay);
                auto last_sample = std::chrono::steady_clock::now();
                while (!stop.stop_requested() && !paused_) {
                    std::unique_lock lock(mutex_);
                    while (!stop.stop_requested() && !paused_) {
                        const auto revision = revision_.load();
                        const auto deadline = last_sample + std::chrono::milliseconds(interval_.load());
                        wake_.wait_until(lock, stop, deadline, [this, revision] { return paused_.load() || revision_.load() != revision; });
                        if (revision_.load() == revision) break;
                    }
                    lock.unlock();
                    if (stop.stop_requested() || paused_) break;
                    last_sample = std::chrono::steady_clock::now();
                    std::vector<std::uint32_t> targets;
                    { std::lock_guard targets_lock(mutex_); targets=fps_targets_; }
                    sampler.set_fps_targets(std::move(targets));
                    auto snapshot = sampler.collect();
                    const bool finished = options.samples && ++emitted >= options.samples;
                    notify(std::move(snapshot), {}, finished);
                    if (finished) return;
                    if (last_sample + std::chrono::milliseconds(interval_.load()) <= std::chrono::steady_clock::now())
                        last_sample = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& error) {
                diagnostics::write(diagnostics::Level::error,"sampler.exception",error.what());
                notify(std::nullopt, error.what(), false);
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, stop, std::chrono::seconds(2), [this] { return paused_.load(); });
            }
            // Sampler/PDH are destroyed before the paused worker sleeps.
        }
    });
}
} // namespace fluxtop
