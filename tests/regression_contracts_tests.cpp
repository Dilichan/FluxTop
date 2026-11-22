#include "fluxtop/model.hpp"
#include "fluxtop/history_identity.hpp"
#include "fluxtop/history_math.hpp"
#include "fluxtop/gpu_process_memory.hpp"
#include "fluxtop/output.hpp"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main() {
    try {
        using namespace fluxtop;
        // Cross-subsystem fixture: two sibling PIDs represent one application in UI,
        // while the original processes remain independent lifetime observations.
        Process parent, child;
        parent.pid=31001; parent.creation_time=1000; parent.name="regression.exe";
        parent.exe_path="c:/flux-tests/regression.exe";
        parent.cpu_percent=0.0; parent.gpu_percent=36.0;
        parent.dedicated_gpu_bytes=300; parent.working_set=1000;
        child=parent; child.pid=31002; child.parent_pid=parent.pid;
        child.creation_time=1001; child.cpu_percent.reset();
        child.gpu_percent.reset(); child.dedicated_gpu_bytes=200;
        child.working_set.reset();
        require(history_identity(parent)==history_identity(child),
            "same-image process lifetimes must map to one history application");
        require(history_score_available(parent) && history_score_available(child),
            "measured GPU memory and CPU zero are real observations");
        const auto groups=aggregate_application_processes({parent,child});
        require(groups.size()==1 && groups[0].member_pids.size()==2,
            "UI aggregation must preserve both member PIDs");
        const auto& app=groups[0];
        require(app.cpu_percent && *app.cpu_percent==0.0 &&
            app.working_set && *app.working_set==1000,
            "unknown child measurements must not erase measured sibling zero/RAM");
        require(app.gpu_percent && *app.gpu_percent==36.0 &&
            app.dedicated_gpu_bytes && *app.dedicated_gpu_bytes==500,
            "grouped GPU engine takes maximum while valid GPU RAM is summed");
        require(gpu_process_memory_consistent(300,400,1000) &&
            gpu_process_memory_consistent(200,400,1000),
            "verified per-PID GPU memory is valid");
        require(!gpu_process_memory_consistent(500,400,1000),
            "aggregate GPU RAM must not be validated as an individual PID");

        Process missing;
        missing.pid=31003; missing.creation_time=2000; missing.name="private.exe";
        Process unrelated=missing; unrelated.pid=31004; unrelated.creation_time=2001;
        require(history_identity(missing)!=history_identity(unrelated),
            "unresolved same-name processes must not share history identity");
        require(!history_score_available(missing),
            "no resource measurement must not create fake zero AutoScore");
        missing.cpu_percent=0.0;
        require(history_score_available(missing),
            "confirmed idle CPU is a valid AutoScore sample");
        missing.cpu_percent.reset(); missing.dedicated_gpu_bytes=0;
        require(history_score_available(missing),
            "confirmed zero GPU RAM is different from unavailable memory");
        missing.dedicated_gpu_bytes.reset();
        const auto unknown_groups=aggregate_application_processes({missing,unrelated});
        require(unknown_groups.size()==2 &&
            !unknown_groups[0].gpu_percent && !unknown_groups[0].dedicated_gpu_bytes,
            "unknown GPU data stays unknown across grouping");

        // PID reuse invalidates earlier CPU baseline, not merely the UI label.
        CpuTracker tracker;
        RawProcess raw;
        raw.pid=40001; raw.creation_time=10; raw.cpu_ticks=0;
        raw.sampled_ns=1000000000;
        auto first=tracker.update({raw},4);
        require(!first[0].cpu_percent,"first CPU observation has no baseline");
        raw.cpu_ticks=10000000; raw.sampled_ns+=1000000000;
        auto second=tracker.update({raw},4);
        require(second[0].cpu_percent && std::abs(*second[0].cpu_percent-25.0)<0.001,
            "CPU rate still uses elapsed time and logical processors");
        raw.creation_time=11; raw.sampled_ns+=1000000000;
        auto reused=tracker.update({raw},4);
        require(!reused[0].cpu_percent && !reused[0].has_baseline,
            "reused PID must reset baseline before scoring");

        // Real measured zero contributes duration; missing sample does not.
        HistoryAccumulator accumulator;
        std::array<std::optional<double>,history_metric_count> values{};
        accumulator.observe(values,1000,2000);
        require(accumulator.uptime_ms==1000 &&
            accumulator.values[static_cast<std::size_t>(HistoryMetric::score)].duration_ms==0,
            "presence counts towards uptime but not towards unknown score duration");
        values[static_cast<std::size_t>(HistoryMetric::score)]=0.0;
        accumulator.observe(values,1000,3000);
        require(accumulator.uptime_ms==2000 &&
            accumulator.values[static_cast<std::size_t>(HistoryMetric::score)].duration_ms==1000 &&
            *accumulator.values[static_cast<std::size_t>(HistoryMetric::score)].average()==0.0,
            "confirmed zero score must accumulate time without changing uptime semantics");

        // The old exported contract must preserve null instead of fake zeros.
        Snapshot json;
        json.timestamp_ms=5000; json.interval_ms=1000;
        json.processes={missing};
        std::ostringstream stream;
        write_jsonl(stream,json,1);
        const auto line=stream.str();
        require(line.find("\\\"schema_version\\\":1")==std::string::npos,
            "JSONL must use actual JSON quotes, not backslash-escaped quotes");
        require(line.find("\"schema_version\":1")!=std::string::npos &&
            line.find("\"cpu_percent\":null")!=std::string::npos &&
            line.find("\"dedicated_gpu_bytes\":null")!=std::string::npos,
            "existing JSONL fields must retain unknown-null contract");

        std::cout << "v37 cross-subsystem regression contracts passed\n";
        return 0;
    } catch(const std::exception& ex) {
        std::cerr << "v37 regression: " << ex.what() << '\n';
        return 1;
    }
}