#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fluxtop {

struct RawProcess {
    std::uint32_t pid{};
    std::string name;
    std::optional<std::string> exe_path;
    std::optional<std::uint64_t> creation_time;
    std::optional<std::uint64_t> cpu_ticks; // 100 ns, kernel + user
    std::int64_t sampled_ns{}; // monotonic, recorded next to GetProcessTimes
    std::optional<std::uint64_t> working_set;
    std::optional<std::uint64_t> private_bytes;
};

struct Process : RawProcess {
    std::optional<double> cpu_percent;
    std::optional<double> gpu_percent;
    std::optional<std::uint64_t> dedicated_gpu_bytes;
    bool has_baseline{};
    double spike_score{};
    std::optional<double> rx_bytes_per_second, tx_bytes_per_second;
};

class CpuTracker {
public:
    std::vector<Process> update(const std::vector<RawProcess>& raw, unsigned processors);
private:
    struct Baseline {
        std::uint64_t creation;
        std::uint64_t ticks;
        std::int64_t sampled_ns;
    };
    std::unordered_map<std::uint32_t, Baseline> previous_;
};

enum class Sort { cpu, ram, gpu, vram, automatic, rx, tx, network };
class SpikeTracker {
public:
    void update(std::vector<Process>& rows);
private:
    std::unordered_map<std::uint32_t, Process> previous_;
};
void sort_processes(std::vector<Process>& rows, Sort sort);
std::optional<std::uint32_t> gpu_instance_pid(const std::wstring& instance);

struct Snapshot {
    std::int64_t timestamp_ms{};
    double collection_ms{};
    double elapsed_ms{};
    unsigned logical_processors{};
    std::size_t inaccessible{};
    std::uint32_t self_pid{};
    std::optional<bool> elevated;
    bool debug_privilege_enabled{};
    std::optional<double> self_cpu_percent;
    std::optional<std::uint64_t> self_working_set;
    std::optional<std::uint64_t> self_private_bytes;
    std::optional<std::uint64_t> self_peak_working_set;
    std::optional<std::uint32_t> self_handles;
    std::string gpu_status; // disabled, unavailable, partial, ready
    std::optional<double> total_cpu_percent, total_gpu_percent;
    std::optional<std::uint64_t> total_ram_used, total_ram_capacity, total_gpu_memory;
    std::optional<double> total_rx_bytes_per_second, total_tx_bytes_per_second;
    std::string network_status;
    double interval_ms{};
    std::optional<std::uint64_t> history_records;
    std::string history_error;
    double history_write_ms{};
    std::vector<Process> processes;
};

} // namespace fluxtop
