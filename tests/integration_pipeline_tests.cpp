#include "fluxtop/model.hpp"
#include "fluxtop/output.hpp"
#include "fluxtop/history_identity.hpp"
#include "fluxtop/disk_attribution.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void need(bool yes,const char* description) {
    if(!yes)throw std::runtime_error(description);
}
std::string encode(const fluxtop::Snapshot& s) {
    std::ostringstream out;
    fluxtop::write_jsonl(out,s,0);
    return out.str();
}
void contains(const std::string& text,const std::string& fragment) {
    need(text.find(fragment)!=std::string::npos,fragment.c_str());
}
}
int main() {
    try {
        using namespace fluxtop;
        constexpr double mib=1048576.0;
        Snapshot observed;
        observed.timestamp_ms=1700000000000LL;
        observed.interval_ms=1000;
        observed.gpu_status="ready";
        observed.network_status="ready";
        observed.disk_io_status="partial-attribution";
        observed.cpu_temperature_c=53.0;
        observed.cpu_temperature_source="hwinfo-cpu-package";
        observed.gpu_temperature_c=60.0;
        observed.disks={{L'C',80.0},{L'D',0.0}};
        Process parent,child,unknown;
        parent.pid=4101;parent.creation_time=100;parent.name="editor.exe";
        parent.exe_path="C:/apps/editor.exe";
        parent.has_baseline=true;
        parent.cpu_percent=20.0;
        parent.working_set=1000000;
        parent.gpu_percent=30.0;
        parent.dedicated_gpu_bytes=2000000;
        parent.rx_bytes_per_second=8000.0;
        parent.tx_bytes_per_second=0.0;
        parent.disk_read_bytes_per_second=8*mib;
        parent.disk_write_bytes_per_second=0.0;
        parent.disk_pressure_percent=50.0;
        parent.disk_volume_bytes_per_second[2]=8*mib;
        parent.disk_volume_pressure_percent[2]=50.0;
        child.pid=4102;child.parent_pid=4101;child.creation_time=101;
        child.name=parent.name;child.exe_path=parent.exe_path;
        child.has_baseline=true;
        child.cpu_percent.reset();child.gpu_percent.reset();
        child.working_set=200000;child.dedicated_gpu_bytes=500000;
        child.disk_read_bytes_per_second=0.0;
        child.disk_write_bytes_per_second=2*mib;
        child.disk_volume_bytes_per_second[3]=2*mib;
        child.disk_volume_pressure_percent[3]=20.0;
        child.disk_pressure_percent=20.0;
        unknown.pid=4103;unknown.creation_time=102;
        unknown.name="protected.exe";
        observed.processes={parent,child,unknown};

        need(history_identity(parent)==history_identity(child),"sibling PIDs have one history identity");
        need(!history_score_available(unknown),"protected process without metrics is not a measured zero");
        need(!disk_io_fully_attributed(8,16,false)&&disk_io_publishable_bytes(8,16,false),
            "partial physical attribution retains verified lower bound");
        need(!disk_io_pid_lifetime_valid(true,1000,999),"stale ETW request must not attach to reused PID");
        const auto serialized_before=encode(observed);
        contains(serialized_before,"\"schema_version\":1");
        contains(serialized_before,"\"disk_measurement_quality\":\"partial\"");
        contains(serialized_before,"\"cpu_temperature_source\":\"hwinfo-cpu-package\"");
        contains(serialized_before,"\"disk_read_bytes_per_second\":8388608.000");
        contains(serialized_before,"\"disk_volume_bytes_per_second\":{\"C\":8388608.000,\"D\":null}");
        contains(serialized_before,"\"disk_volume_bytes_per_second\":{\"C\":null,\"D\":2097152.000}");
        contains(serialized_before,"\"disk_write_bytes_per_second\":0.000");
        contains(serialized_before,"\"disk_read_bytes_per_second\":null");
        auto grouped=aggregate_application_processes(observed.processes);
        need(grouped.size()==2,"two same-executable PIDs must group without merging protected PID");
        auto it=std::find_if(grouped.begin(),grouped.end(),[](const Process& p){return p.name=="editor.exe";});
        need(it!=grouped.end()&&it->member_pids.size()==2,"group retains two member PIDs");
        need(it->disk_read_bytes_per_second&&*it->disk_read_bytes_per_second==8*mib&&
             it->disk_write_bytes_per_second&&*it->disk_write_bytes_per_second==2*mib,
             "physical read/write rates accumulate once across the group");
        need(it->disk_pressure_percent&&*it->disk_pressure_percent==50.0,
             "C and D pressures must not sum as duplicate score categories");
        SpikeTracker scores;
        scores.update(grouped,16000000ULL,8000000ULL);
        need(auto_rank(*it).score>0,"all-metric score must remain available for grouped process");
        auto expected=grouped;
        sort_processes(expected,Sort::automatic);
        for(unsigned mask:{0U,1U,2U,4U,8U,16U,32U,63U}) {
            auto current=grouped;
            sort_processes(current,Sort::automatic,mask);
            for(std::size_t i=0;i<current.size();++i)
                need(current[i].pid==expected[i].pid,"hidden columns changed automatic order");
        }
        need(encode(observed)==serialized_before,"UI scoring or aggregation mutated raw snapshot/JSONL");
        // A new lifetime on the same PID must reset its EWMA, not inherit the old disk burst.
        it->creation_time=200;
        it->cpu_percent.reset();it->working_set.reset();it->gpu_percent.reset();
        it->dedicated_gpu_bytes.reset();it->rx_bytes_per_second.reset();it->tx_bytes_per_second.reset();
        it->disk_pressure_percent.reset();it->disk_read_bytes_per_second.reset();
        it->disk_write_bytes_per_second.reset();
        it->disk_volume_pressure_percent.fill(std::nullopt);
        scores.update(grouped);
        need(auto_rank(*it).score==0.0&&!auto_rank(*it).burst,
             "reused PID must not inherit old automatic pressure");
        // Unavailable sensors / ETW must remain explicit null or unavailable in export.
        observed.disk_io_status="administrator-required";
        observed.cpu_temperature_c.reset();observed.gpu_temperature_c.reset();
        observed.cpu_temperature_source="stale-provider";
        observed.processes[2].disk_read_bytes_per_second.reset();
        const auto missing=encode(observed);
        contains(missing,"\"disk_measurement_quality\":\"unavailable\"");
        contains(missing,"\"cpu_temperature_c\":null");
        contains(missing,"\"gpu_temperature_c\":null");
        contains(missing,"\"cpu_temperature_source\":\"unavailable\"");
        need(missing.find("nan")==std::string::npos&&missing.find("inf")==std::string::npos,
             "JSONL must not expose nonfinite measurements");
        std::cout<<"v45 cross-subsystem integration contracts passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
