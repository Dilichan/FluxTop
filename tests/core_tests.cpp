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
    unknown.rx_bytes_per_second.reset(); unknown.tx_bytes_per_second=10000;
    idle.rx_bytes_per_second=0; idle.tx_bytes_per_second=0;
    busy.rx_bytes_per_second=300; busy.tx_bytes_per_second=10;
    tie.rx_bytes_per_second=20; tie.tx_bytes_per_second=600;
    rows={unknown,idle,busy,tie};
    fluxtop::sort_processes(rows,fluxtop::Sort::rx);
    check(rows[0].pid==4 && rows[1].pid==3 && rows[2].pid==2 && rows[3].pid==1,"Rx sorts receive rate, unknown after zero");
    fluxtop::sort_processes(rows,fluxtop::Sort::tx);
    check(rows[0].pid==1 && rows[1].pid==3 && rows[2].pid==4,"Tx sorts transmit rate");
    fluxtop::sort_processes(rows,fluxtop::Sort::network);
    check(rows[0].pid==3 && rows[1].pid==4 && rows[2].pid==2 && rows[3].pid==1,"combined network sorts Rx+Tx, partial values unknown");
}
void spike_tests() {
    fluxtop::SpikeTracker tracker;
    fluxtop::Process cpu, ram, gpu, vram;
    cpu.pid = 1; ram.pid = 2; gpu.pid = 3; vram.pid = 4;
    std::vector<fluxtop::Process> rows{cpu, ram, gpu, vram};
    for (auto& row : rows) {
        row.creation_time = 1; row.cpu_percent = 0; row.gpu_percent = 0;
        row.working_set = 0; row.dedicated_gpu_bytes = 0;
    }
    tracker.update(rows);
    check(rows[0].spike_score == 0, "first sample has no burst");
    rows[0].cpu_percent = 10;
    rows[1].working_set = 192ULL * 1048576;
    rows[2].gpu_percent = 20;
    rows[3].dedicated_gpu_bytes = 320ULL * 1048576;
    tracker.update(rows);
    fluxtop::sort_processes(rows, fluxtop::Sort::automatic);
    check(rows[0].pid == 4 && rows[1].pid == 3 && rows[2].pid == 2 && rows[3].pid == 1,
        "auto ranks resource bursts with normalized rates");
    tracker.update(rows);
    check(rows[0].spike_score == 0, "steady usage is no longer a new burst");
    check(fluxtop::auto_rank(rows[1]).score >= 1.0, "steady GPU remains eligible after burst");
    tracker.update(rows);
    tracker.update(rows);
    fluxtop::sort_processes(rows, fluxtop::Sort::automatic);
    check(rows[0].pid == 3, "active steady GPU stays above low usage");

    fluxtop::Process cpuHeavy;
    cpuHeavy.pid = 99; cpuHeavy.creation_time = 7; cpuHeavy.cpu_percent = 85;
    std::vector<fluxtop::Process> high{cpuHeavy};
    tracker.update(high);
    check(fluxtop::auto_rank(high[0]).metric == fluxtop::AutoMetric::cpu,
        "high CPU ranks even on the first sample");
    tracker.update(high);
    check(fluxtop::auto_rank(high[0]).score >= 1.0,
        "stable CPU load does not disappear from auto ranking");
    check(fluxtop::auto_rank(high[0],2U).metric == fluxtop::AutoMetric::none,
        "hidden CPU does not influence automatic ranking");

    high[0].cpu_percent=0;
    tracker.update(high);
    check(fluxtop::auto_rank(high[0]).metric == fluxtop::AutoMetric::none,
        "process drops from ranking when sustained load disappears");
    high[0].creation_time=8; high[0].cpu_percent=100;
    tracker.update(high);
    check(high[0].spike_score == 0, "PID reuse cannot create a false burst");
    tracker.update(high = {});
    high = {cpuHeavy}; tracker.update(high);
    check(high[0].spike_score == 0, "exited process baselines are removed");

    fluxtop::Process network;
    network.pid=100; network.creation_time=9;
    network.rx_bytes_per_second=0; network.tx_bytes_per_second=0;
    high={network}; tracker.update(high);
    high[0].rx_bytes_per_second=131072;
    tracker.update(high);
    check(high[0].spike_score == 2.0, "Rx burst detection preserved");
    check(fluxtop::auto_rank(high[0]).burst, "rising Rx value should be orange");
    check(fluxtop::auto_rank(high[0]).metric == fluxtop::AutoMetric::rx,
        "Rx is attributed to the exact network value");
    tracker.update(high);
    check(high[0].spike_score==0 && fluxtop::auto_rank(high[0]).metric == fluxtop::AutoMetric::rx,
        "steady Rx remains ranked after burst");
    check(!fluxtop::auto_rank(high[0]).burst, "stable Rx should be red, not orange");
    high[0].tx_bytes_per_second=196608;
    tracker.update(high);
    check(high[0].spike_score == 3.0, "Tx burst detected independently");
    check(fluxtop::auto_rank(high[0]).metric == fluxtop::AutoMetric::tx,
        "Tx is attributed to the exact network value");
    check(fluxtop::auto_rank(high[0],1U).metric == fluxtop::AutoMetric::none,
        "hidden network column does not drive auto ranking");
}

void output_tests() {
    check(fluxtop::json_escape("a\"\\\n\t\x01") == "\"a\\\"\\\\\\n\\t\\u0001\"", "JSON control escaping");
    check(fluxtop::json_escape("приложение") == "\"приложение\"", "UTF-8 preserved");
    fluxtop::Snapshot s;
    s.gpu_status = "disabled";
    s.total_cpu_percent = 42.0;
    s.total_ram_used = 1024;
    s.total_rx_bytes_per_second = 2048;
    s.network_status = "ready";
    fluxtop::Process p;
    p.pid = 7;
    p.name = "a\"b.exe";
    p.creation_time = 134000000000000001ULL;
    p.cpu_percent = std::numeric_limits<double>::quiet_NaN();
    s.processes = {p, p};
    std::ostringstream out;
    fluxtop::write_jsonl(out, s, 1);
    check(out.str().find("\"elevated\":null,\"debug_privilege_enabled\":false") != std::string::npos, "unknown token and disabled privilege serialize correctly");
    s.elevated = true;
    s.debug_privilege_enabled = true;
    std::ostringstream elevatedOut;
    fluxtop::write_jsonl(elevatedOut, s, 1);
    check(elevatedOut.str().find("\"elevated\":true,\"debug_privilege_enabled\":true") != std::string::npos, "elevated token serializes as JSON booleans");
    check(out.str().find("\"creation_time_100ns\":\"134000000000000001\"") != std::string::npos, "FILETIME precision preserved as string");
    check(out.str().find("\"cpu_percent\":null") != std::string::npos, "unknown/nonfinite JSON is null");
    check(out.str().find("nan") == std::string::npos, "no invalid JSON NaN");
    check(out.str().find("\"process_count\":2") != std::string::npos, "total count independent of top");
    check(out.str().find("\"system\":{\"cpu_percent\":42.000,\"ram_used_bytes\":1024") != std::string::npos,
        "system totals are independent from displayed processes");
    check(out.str().find("\"rx_bytes_per_second\":2048.000") != std::string::npos
        && out.str().find("\"network_status\":\"ready\"") != std::string::npos, "network totals and availability serialize");
    const auto first = out.str().find("\"name\"");
    check(first != std::string::npos && out.str().find("\"name\"", first + 1) == std::string::npos, "top limits exported rows");
}
} // namespace

int main() {
    cpu_tests();
    sort_tests();
    spike_tests();
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
    rejects({"--sort", "disk"});
    for (const auto key : {"rx","tx","network","rx/tx"}) {
        const std::string_view networkArgs[]{"--overlay","--sort",key};
        check(fluxtop::parse_options(networkArgs).sort>=fluxtop::Sort::rx,"network sort keys accepted");
    }
    rejects({"--sort", "gpu", "--no-gpu"});
    rejects({"--unknown"});
    rejects({"--overlay", "--jsonl"});
    rejects({"--tray"});
    const std::string_view trayArgs[]{"--overlay", "--tray", "--no-network"};
    check(fluxtop::parse_options(trayArgs).tray && !fluxtop::parse_options(trayArgs).network, "tray startup and disabled network options");
    rejects({"--overlay", "--top", "0"});
    rejects({"--overlay", "--top", "101"});
    for (const auto count : {"1", "7", "100"}) {
        const std::string_view topArgs[]{"--overlay", "--top", count, "--sort", "auto", "--no-gpu"};
        check(fluxtop::parse_options(topArgs).sort == fluxtop::Sort::automatic, "custom row count and auto without GPU accepted");
    }
    rejects({"--font-size", "20"});
    rejects({"--overlay", "--font-size", "9"});
    rejects({"--overlay", "--font-size", "37"});
    const std::string_view fontArgs[]{"--overlay", "--font-size", "24"};
    check(fluxtop::parse_options(fontArgs).font_explicit && fluxtop::parse_options(fontArgs).font_size == 24,
          "explicit overlay font overrides saved settings");
    const std::string_view overlayArgs[]{"--overlay", "--top", "20", "--sort", "ram"};
    const auto overlay = fluxtop::parse_options(overlayArgs);
    check(overlay.overlay && overlay.top == 20 && overlay.top_explicit && overlay.sort_explicit,
          "overlay options preserve explicit overrides of saved settings");
    if (failures) return 1;
    std::cout << "All core checks passed\n";
    return 0;
}
