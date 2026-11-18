#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fluxtop {

struct RawProcess {
    std::uint32_t pid{};
    std::uint32_t parent_pid{}; // Toolhelp32 parent; verified by creation time before grouping.
    std::string name;
    std::optional<std::string> exe_path;
    std::optional<std::uint64_t> creation_time;
    std::optional<std::uint64_t> cpu_ticks; // 100 ns, kernel + user
    std::int64_t sampled_ns{}; // monotonic, recorded next to GetProcessTimes
    std::optional<std::uint64_t> working_set;
    std::optional<std::uint64_t> private_bytes;
};

enum class AutoMetric : std::uint8_t { none, cpu, ram, gpu, vram, rx, tx };
struct AutoRank {
    double score{};
    AutoMetric metric{AutoMetric::none};
    bool burst{};
};

struct Process : RawProcess {
    std::optional<double> cpu_percent;
    std::optional<double> gpu_percent;
    std::optional<std::uint64_t> dedicated_gpu_bytes;
    bool has_baseline{};
    double spike_score{};
    std::array<double,6> auto_scores{}; // CPU, RAM, GPU, GPU RAM, Rx, Tx
    std::array<double,6> burst_scores{}; // Immediate growth, separate from sustained load
    std::optional<double> rx_bytes_per_second, tx_bytes_per_second;
    std::vector<std::uint32_t> member_pids; // Only populated in UI-level aggregate, never raw history.
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
    void update(std::vector<Process>& rows, std::optional<std::uint64_t> ram_capacity = std::nullopt, std::optional<std::uint64_t> gpu_capacity = std::nullopt);
private:
    std::unordered_map<std::uint32_t, Process> previous_;
};
AutoRank auto_rank(const Process& row, unsigned visible_metrics = 31U);
// Independent per-cell orange flag, using the same threshold as AutoRank.
bool auto_metric_has_burst(const Process& row, AutoMetric metric);
// Actual utilization for colors, independent of weighted AutoScore. Network has no absolute capacity.
std::optional<double> auto_metric_load_percent(const Process& row, AutoMetric metric,
    std::optional<std::uint64_t> ram_capacity, std::optional<std::uint64_t> gpu_capacity);
// Display-only aggregation of verified same-image parent/child processes.
// Original per-PID snapshots and history are never modified.
std::vector<Process> aggregate_application_processes(const std::vector<Process>& rows);
void sort_processes(std::vector<Process>& rows, Sort sort, unsigned visible_metrics = 31U);
// Move the focused process to row zero without changing the order of other rows.
// Return false if pid is zero or not present in this snapshot.
bool prioritize_focused_process(std::vector<Process>& rows, std::uint32_t pid);
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
    std::optional<std::uint64_t> total_ram_used, total_ram_capacity, total_gpu_memory, total_gpu_memory_capacity;
    std::optional<double> cpu_temperature_c, gpu_temperature_c;
    bool cpu_temperature_board{}; // True if CPU value is an explicit motherboard/ACPI CPU zone.
    std::optional<double> total_rx_bytes_per_second, total_tx_bytes_per_second;
    std::string network_status;
    double interval_ms{};
    std::optional<std::uint64_t> history_records;
    std::string history_error;
    double history_write_ms{};
    std::vector<Process> processes;
};

} // namespace fluxtop
