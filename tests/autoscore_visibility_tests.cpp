#include "fluxtop/model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::uint32_t> ids(const std::vector<fluxtop::Process>& rows) {
    std::vector<std::uint32_t> result;
    for (const auto& row:rows) result.push_back(row.pid);
    return result;
}
}
int main() {
    try {
        using namespace fluxtop;
        constexpr std::uint64_t gib=1073741824ULL;
        constexpr double mib=1048576.0;
        require(auto_score_all_metrics==63U,"AutoScore must cover CPU, RAM, GPU, VRAM, network, disk");
        std::vector<Process> measured(8);
        for (std::size_t i=0;i<measured.size();++i) {
            measured[i].pid=static_cast<std::uint32_t>(6000+i);
            measured[i].creation_time=static_cast<std::uint64_t>(1000+i);
            measured[i].name="independent.exe";
        }
        // Disk C is intentionally hotter than Disk D; neither is required to be visible.
        measured[0].disk_volume_pressure_percent[2]=60.0;
        measured[0].disk_volume_bytes_per_second[2]=100.0*mib;
        measured[0].disk_read_bytes_per_second=100.0*mib;
        measured[0].disk_write_bytes_per_second=0.0;
        measured[0].disk_pressure_percent=60.0;
        measured[1].disk_volume_pressure_percent[3]=45.0;
        measured[1].disk_volume_bytes_per_second[3]=80.0*mib;
        measured[1].disk_read_bytes_per_second=0.0;
        measured[1].disk_write_bytes_per_second=80.0*mib;
        measured[1].disk_pressure_percent=45.0;
        measured[2].cpu_percent=18.0;
        measured[3].gpu_percent=35.0;
        measured[4].rx_bytes_per_second=4.0*mib;
        measured[4].tx_bytes_per_second=0.0;
        measured[5].working_set=64ULL*gib;
        measured[6].dedicated_gpu_bytes=4ULL*gib;
        // PID 6007 has no measurements. It must not acquire a synthetic zero disk sample.

        SpikeTracker tracker;
        tracker.update(measured,128ULL*gib,8ULL*gib);
        require(auto_rank(measured[0]).metric==AutoMetric::io &&
                auto_rank(measured[0]).score>=60.0,
                "physical C pressure participates in the full score even when hidden");
        require(auto_rank(measured[1]).metric==AutoMetric::io &&
                auto_rank(measured[1]).score>=45.0,
                "physical D pressure is independent from C");
        require(auto_rank(measured[2]).score>0 && auto_rank(measured[3]).score>0 &&
                auto_rank(measured[4]).score>0 && auto_rank(measured[5]).score>0 &&
                auto_rank(measured[6]).score>0,
                "CPU, GPU, Rx, RAM and VRAM must remain eligible for AutoScore");
        require(!measured[7].disk_pressure_percent && auto_rank(measured[7]).score==0.0,
                "unknown disk measurement must not be fabricated as zero");
        auto reference=measured;
        sort_processes(reference,Sort::automatic,auto_score_all_metrics);
        const auto expected=ids(reference);
        // Legacy visibility masks, including no columns, CPU-only, D-only, and all columns,
        // must not change the sorting result. Overlay now always requests the full mask.
        for(unsigned columns=0;columns<64;++columns) {
            auto owned=measured;
            sort_processes(owned,Sort::automatic,columns);
            require(ids(owned)==expected,"visible metric masks must not influence automatic ordering");
            std::vector<const Process*> refs;
            for(const auto& row:measured)refs.push_back(&row);
            sort_process_refs(refs,Sort::automatic,columns);
            for(std::size_t i=0;i<refs.size();++i)
                require(refs[i]->pid==expected[i],"borrowed and owned AutoSort must agree");
        }
        require(measured[0].disk_volume_bytes_per_second[2] &&
                !measured[0].disk_volume_bytes_per_second[3] &&
                measured[1].disk_volume_bytes_per_second[3] &&
                !measured[1].disk_volume_bytes_per_second[2],
                "display selection and sorting may not mutate volume measurements");
        // The disk category must count only once even when both C and D are measured.
        Process parent,child;
        parent.pid=9001;parent.creation_time=100;parent.name="family.exe";
        parent.exe_path="c:/fixture/family.exe";
        parent.disk_volume_pressure_percent[2]=60.0;
        parent.disk_pressure_percent=60.0;
        parent.disk_read_bytes_per_second=10*mib;
        child=parent;child.pid=9002;child.creation_time=101;child.parent_pid=9001;
        child.disk_volume_pressure_percent.fill(std::nullopt);
        child.disk_volume_pressure_percent[3]=45.0;
        child.disk_pressure_percent=45.0;
        child.disk_read_bytes_per_second=0.0;
        child.disk_write_bytes_per_second=20*mib;
        const auto groups=aggregate_application_processes({parent,child});
        require(groups.size()==1 && groups[0].disk_pressure_percent &&
                std::abs(*groups[0].disk_pressure_percent-60.0)<1e-9,
                "C and D activity must not be added as duplicate disk score categories");
        require(groups[0].disk_read_bytes_per_second && groups[0].disk_write_bytes_per_second &&
                *groups[0].disk_read_bytes_per_second==10*mib &&
                *groups[0].disk_write_bytes_per_second==20*mib,
                "group physical read and write rates must remain independent");
        // PID reuse resets the EWMA/burst baseline and does not inherit old disk activity.
        measured[0].creation_time=2000;
        measured[0].disk_pressure_percent.reset();
        measured[0].disk_volume_pressure_percent.fill(std::nullopt);
        tracker.update(measured,128ULL*gib,8ULL*gib);
        require(auto_rank(measured[0]).score==0.0 && !auto_rank(measured[0]).burst,
                "reused PID without physical disk reading cannot inherit old AutoScore");
        std::cout << "v40 all-metric AutoScore and UI visibility regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "v40 regression: " << error.what() << '\n';
        return 1;
    }
}