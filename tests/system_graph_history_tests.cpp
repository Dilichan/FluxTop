#include "fluxtop/system_graph_history.hpp"
#include "fluxtop/statistics_chart_controls.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace fluxtop;
namespace {
void check(bool pass,const char* text){if(!pass)throw std::runtime_error(text);}
Snapshot sample(std::int64_t at){
    Snapshot s;s.timestamp_ms=at;s.total_cpu_percent=42;s.cpu_temperature_c=55;
    s.total_ram_used=50;s.total_ram_capacity=100;s.total_gpu_percent=12;
    s.total_gpu_memory=25;s.total_gpu_memory_capacity=100;
    s.total_rx_bytes_per_second=125000;s.total_tx_bytes_per_second=125000;
    s.disks={{L'C',35,45}};
    s.cpu_topology.cores={{0,0,0,true,61,2,false}};
    s.cpu_logical_usage={{0,0,0,true,40},{0,1,0,true,60}};return s;
}
}
int main(){try{
    auto history=std::make_shared<SystemGraphHistory>();
    // No page or client window exists during this collection period.
    constexpr std::int64_t start=1800000000000;
    for(unsigned i=0;i<=1800;++i)check(history->append(sample(start+i*1000)),"record samples independently of window visibility");
    check(history->points().size()==1801,"retain the whole 30-minute timeline at normal cadence");
    const auto twenty_five_minutes_ago=history->points().back().time-25*60*1000;
    const auto found=std::find_if(history->points().begin(),history->points().end(),[&](const auto& p){return p.time==twenty_five_minutes_ago;});
    check(found!=history->points().end() && found->values[0]==42 && found->cpu_temperature==55,"opening statistics can immediately access a real point 25 minutes ago");
    check(found->values[1]==50 && found->values[2]==2 && found->values[3]==12 && found->values[4]==25 && found->disks[2]==35 && found->disk_temperatures[2]==45,
        "cache preserves all existing system metrics and disk temperatures");
    auto page=history;page.reset(); // Closing a page drops only its ownership.
    check(history->points().size()==1801,"closing and reopening the page must not clear history");
    const auto before=history->points().size();
    check(!history->append(sample(start+1800000)) && !history->append(sample(start)),"duplicate/older UI delivery cannot double count or corrupt the timeline");
    check(history->points().size()==before,"a newly attached page does not replay duplicate points");
    history->append(sample(start+1801000));
    check(history->points().front().time==start+1000,"prune by elapsed time rather than page lifetime");
    const auto& latest=history->points().back();
    check(cpu_chart_value(cpu_core_chart_series(latest.cpu,{0,0},false)[0],latest.cpu,{})==50 &&
        cpu_chart_value(cpu_core_chart_series(latest.cpu,{0,0},true)[0],latest.cpu,{})==61,"core history remains usable by the existing graph series");
    const auto frozen=history->points();history->append(sample(start+1802000));
    check(frozen.back().time==start+1801000,"new background samples do not mutate a paused copy");
    SystemGraphHistory fast;
    for(unsigned i=0;i<=8000;++i)fast.append(sample(start+i*250));
    check(fast.points().size()==7201 && fast.points().back().time-fast.points().front().time==SystemGraphHistory::retention_ms,
        "250 ms collection still preserves 30 minutes with a bounded point count");
    check(chart_system_duration(3)>25*60*1000,"view range can expose a point 25 minutes ago");
    std::cout<<"Monitor-owned 30-minute graph history, hidden/reopened views and retention passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
