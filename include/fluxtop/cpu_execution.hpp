#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <span>

namespace fluxtop {
std::optional<std::pair<std::uint32_t,std::uint32_t>> cpu_switch_threads(unsigned version,std::span<const std::uint8_t> payload);
struct CpuExecutionValue {
    unsigned group{},number{};
    std::uint64_t runtime_ns{}; // Observed lower bound; never an imputed interval.
    std::optional<double> percent; // Only present for a fully covered logical CPU/window.
    double observed_percent{}; // Lower bound for a partial-but-unbroken trace, never an exact claim.
};
struct CpuProcessExecution {
    std::string status{"initializing"};
    std::uint64_t begin_qpc{},end_qpc{},frequency{};
    std::vector<CpuExecutionValue> logical;
};
struct CpuExecutionIdentity {
    std::uint32_t pid{};
    std::uint64_t creation{};
    bool operator==(const CpuExecutionIdentity&) const = default;
};
struct CpuExecutionWindow {
    struct Process { CpuExecutionIdentity identity; CpuProcessExecution execution; };
    std::string status{"baseline"};
    std::uint64_t begin_qpc{},end_qpc{},frequency{},unknown_ticks{},switches{};
    std::vector<Process> processes;
    std::vector<CpuExecutionIdentity> known_processes;
    std::vector<std::pair<unsigned,unsigned>> topology;
    std::vector<bool> complete;
    CpuProcessExecution for_process(CpuExecutionIdentity identity) const;
};
// Single-consumer deterministic QPC state machine. No Win32/UI dependency.
// Mutations happen on the ETW consumer thread, including window boundaries.
class CpuExecutionTracker {
public:
    struct Limits { std::size_t threads=65536,processes=8192,cells=131072; };
    CpuExecutionTracker(std::vector<std::pair<unsigned,unsigned>> topology,std::uint64_t frequency,Limits limits);
    CpuExecutionTracker(std::vector<std::pair<unsigned,unsigned>> topology,std::uint64_t frequency);
    ~CpuExecutionTracker();
    void process_start(std::uint32_t pid,std::uint64_t creation,std::uint64_t at);
    void process_end(std::uint32_t pid,std::uint64_t at);
    bool has_process(std::uint32_t pid) const;
    void thread_start(std::uint32_t tid,std::uint32_t pid,std::uint64_t at,bool rundown=false);
    void thread_end(std::uint32_t tid,std::uint32_t pid,std::uint64_t at);
    void context_switch(std::size_t cpu,std::uint32_t old_tid,std::uint32_t new_tid,std::uint64_t at);
    void invalidate(std::string reason);
    CpuExecutionWindow finish(std::uint64_t at);
    std::size_t thread_count() const;
    const std::string& failure_detail() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void merge_cpu_execution(CpuProcessExecution& target,const CpuProcessExecution& part,bool first);
}
