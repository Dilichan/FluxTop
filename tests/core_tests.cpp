#include "fluxtop/model.hpp"
#include "fluxtop/overlay_layout.hpp"
#include "fluxtop/gpu_process_memory.hpp"
#include "fluxtop/options.hpp"
#include "fluxtop/output.hpp"

#include <algorithm>
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
        std::cerr << "FAIL: expected invalid_argument for:";
        for (const auto arg : args) std::cerr << ' ' << arg;
        std::cerr << '\n';
        ++failures;
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
    // The collector transfers ownership of name/path strings into Process rows.
    sample.name="example.exe";
    sample.exe_path="c:\\test\\example.exe";
    std::vector<fluxtop::RawProcess> batch{sample};
    result=tracker.update(std::move(batch),4);
    check(result[0].name==sample.name && result[0].exe_path==sample.exe_path,
        "moving RawProcess retains process identity");
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
    std::vector<const fluxtop::Process*> refs;
    for (const auto& row:rows) refs.push_back(&row);
    fluxtop::sort_process_refs(refs,fluxtop::Sort::network);
    check(refs.size()==rows.size() && refs[0]->pid==3 && refs[1]->pid==4 &&
        refs[2]->pid==2 && refs[3]->pid==1,"borrowed-row order matches owned-row order");
    for (const auto mode:{fluxtop::Sort::cpu,fluxtop::Sort::ram,fluxtop::Sort::gpu,
        fluxtop::Sort::vram,fluxtop::Sort::automatic,fluxtop::Sort::rx,fluxtop::Sort::tx}) {
        auto owned=rows;
        fluxtop::sort_processes(owned,mode);
        auto borrowed=refs;
        fluxtop::sort_process_refs(borrowed,mode);
        const bool same=std::equal(owned.begin(),owned.end(),borrowed.begin(),
            [](const auto& a,const auto* b){return a.pid==b->pid;});
        check(same,"borrowed-row comparator remains consistent for each sort mode");
    }
    check(fluxtop::prioritize_focused_process(refs,2) && refs.front()->pid==2,
        "borrowed-row foreground rotation");
    check(!fluxtop::prioritize_focused_process(refs,9999),"missing foreground keeps row ordering");
}
void spike_tests() {
    constexpr std::uint64_t gib = 1073741824ULL;
    fluxtop::SpikeTracker tracker;
    fluxtop::Process gpu, cpu, memory, network;
    gpu.pid=10; gpu.creation_time=1; gpu.gpu_percent=80;
    cpu.pid=11; cpu.creation_time=1; cpu.cpu_percent=18;
    memory.pid=12; memory.creation_time=1; memory.working_set=8*gib;
    network.pid=13; network.creation_time=1; network.rx_bytes_per_second=0;
    std::vector<fluxtop::Process> rows{memory,cpu,gpu,network};
    tracker.update(rows,128*gib,8*gib);
    fluxtop::sort_processes(rows,fluxtop::Sort::automatic);
    check(rows[0].pid==10 && rows[1].pid==11 && rows[2].pid==12,
        "sustained GPU and active CPU outrank allocated memory on 128 GiB host");
    check(fluxtop::auto_rank(rows[0]).metric==fluxtop::AutoMetric::gpu,
        "GPU load is the reason for a high ranking");
    check(fluxtop::auto_rank(rows[2]).metric==fluxtop::AutoMetric::none,
        "low physical RAM share does not force a red label");
    tracker.update(rows,128*gib,8*gib);
    check(fluxtop::auto_rank(rows[0]).metric==fluxtop::AutoMetric::gpu
        && !fluxtop::auto_rank(rows[0]).burst,"stable GPU stays ranked red");
    for (auto& p:rows) if(p.pid==11) p.cpu_percent=28;
    tracker.update(rows,128*gib,8*gib);
    auto cpuRow=std::find_if(rows.begin(),rows.end(),[](const auto& p){return p.pid==11;});
    check(cpuRow != rows.end() && fluxtop::auto_rank(*cpuRow).burst
        && fluxtop::auto_rank(*cpuRow).metric==fluxtop::AutoMetric::cpu,
        "CPU step change gets orange even if sustained CPU is already high");
    tracker.update(rows,128*gib,8*gib);
    tracker.update(rows,128*gib,8*gib);
    cpuRow=std::find_if(rows.begin(),rows.end(),[](const auto& p){return p.pid==11;});
    check(cpuRow != rows.end() && !fluxtop::auto_rank(*cpuRow).burst
        && fluxtop::auto_rank(*cpuRow).metric==fluxtop::AutoMetric::cpu,
        "orange eventually returns to sustained red");
    for (auto& p:rows) if(p.pid==13) p.rx_bytes_per_second=4*1048576.0;
    tracker.update(rows,128*gib,8*gib);
    auto netRow=std::find_if(rows.begin(),rows.end(),[](const auto& p){return p.pid==13;});
    check(netRow != rows.end() && fluxtop::auto_rank(*netRow).burst
        && fluxtop::auto_rank(*netRow).metric==fluxtop::AutoMetric::rx,
        "Rx growth becomes orange and affects the rating");
    check(netRow != rows.end() && fluxtop::auto_rank(*netRow,15U).metric==fluxtop::AutoMetric::none,
        "hidden network column cannot control automatic ranking");
    fluxtop::Process combined, solitary;
    combined.pid=20; combined.creation_time=1; combined.cpu_percent=20; combined.gpu_percent=40;
    solitary.pid=21; solitary.creation_time=1; solitary.cpu_percent=20;
    std::vector<fluxtop::Process> pair{solitary,combined};
    tracker.update(pair,128*gib,8*gib);
    check(fluxtop::auto_rank(pair[1]).score > fluxtop::auto_rank(pair[0]).score,
        "two independent heavy resources receive bounded complementary points");
    for (auto& p:rows) if(p.pid==11) {p.cpu_percent=0; p.creation_time=2;}
    tracker.update(rows,128*gib,8*gib);
    cpuRow=std::find_if(rows.begin(),rows.end(),[](const auto& p){return p.pid==11;});
    check(cpuRow != rows.end() && !fluxtop::auto_rank(*cpuRow).burst,
        "PID reuse must not generate a false change event");
}
void color_load_tests() {
    constexpr std::uint64_t gib = 1073741824ULL;
    fluxtop::Process p;
    p.cpu_percent = 4.0;
    p.gpu_percent = 73.0;
    p.working_set = 4*gib;
    p.dedicated_gpu_bytes = 4*gib + gib/2;
    const auto near = [](std::optional<double> v, double target) {
        return v && std::abs(*v-target) < 0.001;
    };
    check(near(fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::cpu,128*gib,8*gib),4.0),
        "CPU color is based on raw machine utilization, not weighted score");
    check(near(fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::gpu,128*gib,8*gib),73.0),
        "GPU color is based on raw engine utilization");
    check(near(fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::ram,128*gib,8*gib),3.125),
        "RAM color uses installed physical memory capacity");
    check(near(fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::vram,128*gib,8*gib),56.25),
        "VRAM color uses GPU memory capacity, not AutoScore weight");
    check(!fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::ram,std::nullopt,8*gib),
        "unknown RAM capacity gives unknown color, not a fabricated percentage");
    check(!fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::vram,128*gib,0),
        "zero GPU memory capacity yields unknown color");
    check(!fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::rx,128*gib,8*gib),
        "network rates have no percentage denominator; orange bursts remain separate");
    p.dedicated_gpu_bytes = 12*gib;
    check(near(fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::vram,128*gib,8*gib),100.0),
        "per-process GPU usage over reported capacity is capped for color purposes");
}
void orange_burst_visibility_tests() {
    constexpr std::uint64_t gib = 1073741824ULL;
    fluxtop::SpikeTracker tracker;
    fluxtop::Process p;
    p.pid = 801; p.creation_time = 42; p.gpu_percent = 40.0;
    std::vector<fluxtop::Process> rows{p};
    tracker.update(rows, 128*gib, 8*gib);
    check(!fluxtop::auto_rank(rows[0]).burst, "first reading has no orange alert");
    rows[0].gpu_percent = 43.0;
    tracker.update(rows, 128*gib, 8*gib);
    const auto small = fluxtop::auto_rank(rows[0]);
    check(small.burst && small.metric == fluxtop::AutoMetric::gpu,
        "GPU rise of 3 percentage points is orange, below old 20-point limit");
    check(small.score < 50.0, "small visual alert does not inflate ranking with a large bonus");
    tracker.update(rows, 128*gib, 8*gib);
    check(!fluxtop::auto_rank(rows[0]).burst, "small burst fades on the next unchanged reading");
    rows[0].gpu_percent = 43.5;
    tracker.update(rows, 128*gib, 8*gib);
    check(!fluxtop::auto_rank(rows[0]).burst, "tiny GPU jitter does not flash orange");
    rows[0].gpu_percent = 53.5;
    tracker.update(rows, 128*gib, 8*gib);
    check(fluxtop::auto_rank(rows[0]).burst, "large GPU burst is orange");
    tracker.update(rows, 128*gib, 8*gib);
    check(fluxtop::auto_rank(rows[0]).burst, "large burst remains visible for another sample");
    rows[0].creation_time = 43;
    rows[0].gpu_percent = 85.0;
    tracker.update(rows, 128*gib, 8*gib);
    check(!fluxtop::auto_rank(rows[0]).burst, "PID reuse cannot create false orange alerts");
    check(!fluxtop::auto_rank(rows[0], 3U).burst, "hidden GPU cannot trigger orange alerts");

    fluxtop::SpikeTracker movement;
    fluxtop::Process gpu, cpu;
    gpu.pid=802; gpu.creation_time=1; gpu.gpu_percent=40.0;
    cpu.pid=803; cpu.creation_time=1; cpu.cpu_percent=20.0;
    std::vector<fluxtop::Process> ranking{gpu,cpu};
    movement.update(ranking, 128*gib, 8*gib);
    fluxtop::sort_processes(ranking, fluxtop::Sort::automatic);
    check(ranking[0].pid==803, "CPU initially outranks GPU");
    for (auto& row : ranking) if (row.pid==803) row.cpu_percent=5.0;
    movement.update(ranking, 128*gib, 8*gib);
    fluxtop::sort_processes(ranking, fluxtop::Sort::automatic);
    check(ranking[0].pid==802 && !fluxtop::auto_rank(ranking[0]).burst,
        "rank improvement caused by another process slowing down is not orange");
}
void per_metric_colors_and_bursts_tests() {
    constexpr std::uint64_t gib = 1073741824ULL;
    fluxtop::Process p;
    p.gpu_percent=15.0;
    p.dedicated_gpu_bytes=6*gib;
    p.working_set=64*gib;
    const auto gpu = fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::gpu,128*gib,8*gib);
    const auto vram = fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::vram,128*gib,8*gib);
    const auto ram = fluxtop::auto_metric_load_percent(p,fluxtop::AutoMetric::ram,128*gib,8*gib);
    check(gpu && *gpu==15.0 && vram && *vram==75.0 && ram && *ram==50.0,
        "independent cell percentages are raw loads, not AutoScore weights");
    p.burst_scores = {0.0,6.0,4.9,5.0,0.0,7.0};
    check(fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::ram),
        "RAM burst highlights RAM even when GPU dominates ranking");
    check(!fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::gpu),
        "GPU without a significant burst is not orange");
    check(fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::vram),
        "GPU RAM burst highlights independently of RAM and GPU");
    check(!fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::rx)
        && fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::tx),
        "Rx/Tx burst tinting distinguishes reception from transmission");
    check(!fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::none),
        "invalid metric has no burst");
    p.burst_scores[2] = std::numeric_limits<double>::quiet_NaN();
    check(!fluxtop::auto_metric_has_burst(p,fluxtop::AutoMetric::gpu),
        "nonfinite burst cannot trigger coloring");
}
void gpu_process_memory_tests() {
    constexpr double gib=1073741824.0;
    fluxtop::GpuMemoryCounterAccumulator accumulator;
    const auto first=L"pid_50836_luid_0x00000000_0x0000f7a1_phys_0";
    const auto second=L"pid_50836_luid_0x00000000_0x00012755_phys_0";
    check(accumulator.add(first,2.447*gib),"first GPU adapter PDH instance is valid");
    check(accumulator.add(second,0.0),"different GPU adapter with zero usage is valid");
    check(!accumulator.add(first,2.447*gib),"same GPU adapter must not double count");
    check(accumulator.add(L"pid_999_luid_0x00000000_0x0000f7a1_phys_0",0.5*gib),
        "another process memory is accounted independently");
    check(!accumulator.add(L"pid_invalid_luid_0x00000000_phys_0",42.0),
        "invalid PID is not attributed");
    check(!accumulator.add(L"pid_50836_luid_invalid_phys_0",std::numeric_limits<double>::quiet_NaN()),
        "NaN must not corrupt memory totals");
    check(!accumulator.add(L"pid_50836_luid_invalid_phys_0",std::ldexp(1.0,64)),
        "overlarge bytes must not overflow uint64");
    const auto results=std::move(accumulator).take();
    check(results.size()==2 && results.contains(50836U) && results.contains(999U),
        "PDH LUID totals are grouped by PID only after validating instances");
    if (const auto found=results.find(50836U); found!=results.end())
        check(std::abs(static_cast<double>(found->second)/gib-2.447)<0.00001,
            "GPU memory across two LUIDs should be 2.447 GiB, not zero or duplicated");
}
void focused_process_tests() {
    fluxtop::Process a,b,c;
    a.pid=101; a.auto_scores[2]=90.0;
    b.pid=102; b.auto_scores[2]=60.0;
    c.pid=103; c.auto_scores[2]=5.0;
    std::vector<fluxtop::Process> rows{c,b,a};
    fluxtop::sort_processes(rows,fluxtop::Sort::automatic);
    check(rows[0].pid==101 && rows[1].pid==102 && rows[2].pid==103,
        "initial automatic ranking is not affected by foreground handling");
    check(fluxtop::prioritize_focused_process(rows,103),"focus PID is found");
    check(rows[0].pid==103 && rows[1].pid==101 && rows[2].pid==102,
        "focused low-load process is first; other ranks preserve order");
    check(fluxtop::prioritize_focused_process(rows,101),"changing foreground PID is found");
    check(rows[0].pid==101 && rows[1].pid==103 && rows[2].pid==102,
        "new focus is first, stable remainder is preserved");
    check(!fluxtop::prioritize_focused_process(rows,0),"zero PID leaves ranking untouched");
    check(!fluxtop::prioritize_focused_process(rows,999),"unsampled PID leaves ranking untouched");
    check(rows[0].pid==101 && rows[1].pid==103 && rows[2].pid==102,
        "unavailable foreground has no effect");
    check(fluxtop::prioritize_focused_process(rows,101),"already first PID remains valid");
}
void multiprocess_aggregation_tests() {
    using fluxtop::Process;
    auto make=[](std::uint32_t pid,std::uint32_t parent,std::uint64_t birth,
                 const char* path,double cpu,std::uint64_t ram,double gpu,std::uint64_t vram) {
        Process p;
        p.pid=pid; p.parent_pid=parent; p.creation_time=birth;
        p.exe_path=std::string(path); p.name="browser.exe";
        p.cpu_percent=cpu; p.working_set=ram; p.gpu_percent=gpu;
        p.dedicated_gpu_bytes=vram;
        return p;
    };
    constexpr std::uint64_t mib=1048576ULL;
    auto root=make(100,0,10,"c:/browser/browser.exe",2,400*mib,3,0);
    auto gpu=make(101,100,11,"c:/browser/browser.exe",4,200*mib,45,500*mib);
    auto tab=make(102,101,12,"c:/browser/browser.exe",1,300*mib,2,150*mib);
    auto detached=make(200,0,9,"c:/browser/browser.exe",8,100*mib,5,80*mib);
    auto older=make(104,100,5,"c:/browser/browser.exe",9,50*mib,7,60*mib);
    auto other=make(103,100,13,"d:/tools/browser.exe",6,70*mib,9,90*mib);
    // All five processes of the same installed executable must appear in ONE
    // application row, even if their PPID trees have multiple independent roots.
    auto grouped=fluxtop::aggregate_application_processes({gpu,tab,root,detached,other,older});
    check(grouped.size()==2,"same EXE path merges detached roots; different paths stay apart");
    const auto it=std::find_if(grouped.begin(),grouped.end(),[](const auto& p){
        return p.exe_path && *p.exe_path=="c:/browser/browser.exe";
    });
    check(it!=grouped.end(),"application identity is canonical full executable path");
    if (it!=grouped.end()) {
        check(it->cpu_percent && *it->cpu_percent==24,"CPU across all same-path workers");
        check(it->working_set && *it->working_set==1050*mib,
            "RAM across all five workers, including the detached roots");
        check(it->dedicated_gpu_bytes && *it->dedicated_gpu_bytes==790*mib,
            "GPU memory across all five workers");
        check(it->gpu_percent && *it->gpu_percent==45,"GPU load is maximum, not sum");
        check(it->member_pids.size()==5,"five distinct PIDs are represented exactly once");
        auto focused=grouped;
        check(fluxtop::prioritize_focused_process(focused,102)
            && focused.front().exe_path==it->exe_path,"child foreground pins whole application");
        check(fluxtop::prioritize_focused_process(focused,200)
            && focused.front().exe_path==it->exe_path,"detached foreground pins same application");
    }
    // When the executable path is unavailable, only confirmed parent-child
    // lineage may merge; a matching executable name alone is insufficient.
    auto no_path_parent=make(501,0,100,"c:/unused.exe",1,50*mib,0,0);
    auto no_path_child=make(502,501,101,"c:/unused.exe",2,60*mib,0,0);
    auto no_path_unrelated=make(503,0,100,"c:/unused.exe",3,70*mib,0,0);
    no_path_parent.exe_path.reset(); no_path_child.exe_path.reset();
    no_path_unrelated.exe_path.reset();
    const auto cautious=fluxtop::aggregate_application_processes(
        {no_path_child,no_path_parent,no_path_unrelated});
    check(cautious.size()==2,"pathless related processes merge; unrelated names stay separate");
    const auto matched=std::find_if(cautious.begin(),cautious.end(),[](const auto& p){
        return p.pid==501;
    });
    check(matched!=cautious.end() && matched->working_set
        && *matched->working_set==110*mib,"pathless parent-child memory is correct");
    std::vector<Process> reversed_chain;
    for (std::uint32_t i=64;i>0;--i) {
        Process p;
        p.pid=1000+i; p.parent_pid=999+i; p.creation_time=i;
        p.name="same.exe"; p.working_set=1;
        reversed_chain.push_back(std::move(p));
    }
    const auto compressed=fluxtop::aggregate_application_processes(reversed_chain);
    check(compressed.size()==1 && compressed.front().member_pids.size()==64,
        "long reversed pathless chain resolves to a single root");
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

void overlay_geometry_tests() {
    using fluxtop::calculate_overlay_columns;
    const auto& config = fluxtop::overlay_layout;
    constexpr float font = 16.0f;
    const auto all = calculate_overlay_columns(font, 31U, false, true);
    check(std::abs(all.left[0] - (config.left_padding + config.icon_width * font)) < .001f,
        "icon reservation comes from the centralized geometry settings");
    check(std::abs(all.right[0] - all.left[0] - config.application_width * font) < .001f,
        "application-name width comes from the centralized settings");
    for (unsigned index = 0; index < 5; ++index) {
        check(std::abs((all.right[index+1] - all.left[index+1]) - config.metric_widths[index]*font) < .001f,
            "a metric's width is exact, without an undocumented minimum");
        check(std::abs(all.left[index+1] - all.right[index] - config.column_gap*font) < .001f,
            "all inter-column gaps use the same coefficient");
    }
    check(std::abs(all.total_width - all.right[5] - config.right_padding) < .001f,
        "overlay width matches the last visible column exactly");

    const auto noIcons = calculate_overlay_columns(font, 31U, false, false);
    check(std::abs((all.total_width - noIcons.total_width) - config.icon_width*font) < .001f,
        "hiding icons reduces table width by the configured icon slot");

    const auto hidden = calculate_overlay_columns(font, (1U<<0) | (1U<<3), false, false);
    check(hidden.right[2] == 0.0f && hidden.right[3] == 0.0f && hidden.right[5] == 0.0f,
        "hidden metrics have no allocated geometry");
    check(std::abs(hidden.left[4] - hidden.right[1] - config.column_gap*font) < .001f,
        "hidden metric columns do not introduce extra gaps");
    check(std::abs(hidden.total_width - hidden.right[4] - config.right_padding) < .001f,
        "table width uses the last active metric rather than a fixed template");

    const auto summary = calculate_overlay_columns(font, 31U, true, true);
    check(summary.left[0] == 0.0f && summary.right[0] == 0.0f,
        "summary-only view has no application column");
    check(std::abs(summary.left[1] - config.left_padding) < .001f,
        "summary-only view starts at the left padding without an unnecessary gap");
    check(std::abs(summary.total_width - summary.right[5] - config.right_padding) < .001f,
        "summary-only width is produced by the same layout calculation");
}
int main() {
    overlay_geometry_tests();
    cpu_tests();
    sort_tests();
    focused_process_tests();
    multiprocess_aggregation_tests();
    spike_tests();
    orange_burst_visibility_tests();
    per_metric_colors_and_bursts_tests();
    color_load_tests();
    gpu_process_memory_tests();
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
    rejects({"--overlay", "--font-size", "4"});
    rejects({"--overlay", "--font-size", "43"});
    for(const auto size : {"5", "9", "37", "42"}) {
        const std::string_view valid[]{"--overlay", "--font-size", size};
        check(fluxtop::parse_options(valid).font_explicit,
            "the entire new 5..42 font-size interval is valid");
    }
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
