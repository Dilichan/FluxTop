#include "fluxtop/model.hpp"
#include "fluxtop/options.hpp"
#include "fluxtop/output.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void rejects(std::initializer_list<std::string_view> args) {
    try {
        (void)fluxtop::parse_options(std::span(args.begin(), args.size()));
        check(false, "invalid arguments must throw");
    } catch (const std::invalid_argument&) {}
}
void cpu_tests() {
    fluxtop::CpuTracker tracker;
    fluxtop::RawProcess sample;
    sample.pid = 10;
    sample.creation_time = 1;
    sample.cpu_ticks = 0;
    sample.sampled_ns = 1000000000;
    auto result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent, "first sample is unknown");
    sample.cpu_ticks = 10000000; // one CPU-second over one wall-second
    sample.sampled_ns += 1000000000;
    result = tracker.update({sample}, 4);
    check(result[0].cpu_percent && std::abs(*result[0].cpu_percent - 25.0) < 0.0001, "one core of four is 25%");
    sample.creation_time = 2;
    sample.cpu_ticks = 1;
    sample.sampled_ns += 1000000000;
    result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent && !result[0].has_baseline, "PID reuse resets baseline");
    sample.sampled_ns += 1000000000;
    result = tracker.update({sample}, 4);
    check(result[0].cpu_percent == 0.0, "known idle is zero");
    sample.cpu_ticks = 0;
    sample.sampled_ns += 1000000000;
    result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent, "counter regression is unknown");
    (void)tracker.update({}, 4);
    result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent, "exited processes are evicted");
    result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent, "zero elapsed time is unknown");
    sample.sampled_ns += 1000000000;
    result = tracker.update({sample}, 0);
    check(!result[0].cpu_percent, "zero processors cannot divide");
    sample.cpu_ticks.reset();
    result = tracker.update({sample}, 4);
    check(!result[0].cpu_percent, "access loss is unknown");
}
void sort_tests() {
    fluxtop::Process unknown, idle, busy, tie;
    unknown.pid = 1;
    idle.pid = 2; idle.cpu_percent = 0;
    busy.pid = 4; busy.cpu_percent = 25;
    tie.pid = 3; tie.cpu_percent = 25;
    std::vector<fluxtop::Process> rows{unknown, busy, idle, tie};
    fluxtop::sort_processes(rows, fluxtop::Sort::cpu);
    check(rows[0].pid == 3 && rows[1].pid == 4 && rows[2].pid == 2 && rows[3].pid == 1,
          "descending sort, PID tie-break, unknown after zero");
}
void output_tests() {
    check(fluxtop::json_escape("a\"\\\n\t\x01") == "\"a\\\"\\\\\\n\\t\\u0001\"", "JSON control escaping");
    check(fluxtop::json_escape("приложение") == "\"приложение\"", "UTF-8 preserved");
    fluxtop::Snapshot s;
    s.gpu_status = "disabled";
    fluxtop::Process p;
    p.pid = 7;
    p.name = "a\"b.exe";
    p.creation_time = 134000000000000001ULL;
    p.cpu_percent = std::numeric_limits<double>::quiet_NaN();
    s.processes = {p, p};
    std::ostringstream out;
    fluxtop::write_jsonl(out, s, 1);
    check(out.str().find("\"creation_time_100ns\":\"134000000000000001\"") != std::string::npos, "FILETIME precision preserved as string");
    check(out.str().find("\"cpu_percent\":null") != std::string::npos, "unknown/nonfinite JSON is null");
    check(out.str().find("nan") == std::string::npos, "no invalid JSON NaN");
    check(out.str().find("\"process_count\":2") != std::string::npos, "total count independent of top");
    const auto first = out.str().find("\"name\"");
    check(first != std::string::npos && out.str().find("\"name\"", first + 1) == std::string::npos, "top limits exported rows");
}
} // namespace

int main() {
    cpu_tests();
    sort_tests();
    output_tests();
    check(fluxtop::gpu_instance_pid(L"pid_123_luid_0x000_0_phys_0_eng_1") == 123U, "parse GPU PID");
    check(!fluxtop::gpu_instance_pid(L"pid_42949672960_luid_0"), "overflow rejected");
    check(!fluxtop::gpu_instance_pid(L"pid__luid_0"), "missing PID rejected");
    check(!fluxtop::gpu_instance_pid(L"total"), "nonprocess instance rejected");
    check(!fluxtop::gpu_instance_pid(L"pid_12x_luid"), "invalid PID boundary rejected");
    const std::string_view args[]{"--samples", "2", "--top", "0", "--sort", "ram", "--no-gpu"};
    const auto options = fluxtop::parse_options(args);
    check(options.samples == 2 && options.top == 0 && !options.gpu && options.sort == fluxtop::Sort::ram, "valid options");
    rejects({"--samples", "-1"});
    rejects({"--samples", "42949672960"});
    rejects({"--samples", "1x"});
    rejects({"--samples"});
    rejects({"--interval-ms", "249"});
    rejects({"--interval-ms", "60001"});
    rejects({"--sort", "network"});
    rejects({"--sort", "gpu", "--no-gpu"});
    rejects({"--unknown"});
    if (failures) return 1;
    std::cout << "All core checks passed\n";
    return 0;
}
