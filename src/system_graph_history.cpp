#include "fluxtop/system_graph_history.hpp"

namespace fluxtop {
bool SystemGraphHistory::append(const Snapshot& snapshot,std::uint64_t tick,std::optional<std::int64_t> wall_ms){
    if(!points_.empty() && snapshot.timestamp_ms<=points_.back().time)return false;
    SystemChartPoint point;point.time=snapshot.timestamp_ms;
    point.values[0]=snapshot.total_cpu_percent;
    if(snapshot.total_ram_used && snapshot.total_ram_capacity && *snapshot.total_ram_capacity)
        point.values[1]=100.0*static_cast<double>(*snapshot.total_ram_used)/static_cast<double>(*snapshot.total_ram_capacity);
    if(snapshot.total_rx_bytes_per_second && snapshot.total_tx_bytes_per_second)
        point.values[2]=8.0*(*snapshot.total_rx_bytes_per_second+*snapshot.total_tx_bytes_per_second)/1000000.0;
    point.values[3]=snapshot.total_gpu_percent;
    if(snapshot.total_gpu_memory && snapshot.total_gpu_memory_capacity && *snapshot.total_gpu_memory_capacity)
        point.values[4]=100.0*static_cast<double>(*snapshot.total_gpu_memory)/static_cast<double>(*snapshot.total_gpu_memory_capacity);
    for(const auto& disk:snapshot.disks)if(disk.letter>=L'A' && disk.letter<=L'Z'){
        point.disks[disk.letter-L'A']=disk.active_percent;
        point.disk_temperatures[disk.letter-L'A']=disk.temperature_c;
    }
    point.cpu_temperature=snapshot.cpu_temperature_c;
    point.cpu=cpu_chart_sample(snapshot,tick,wall_ms);
    points_.push_back(std::move(point));
    while(!points_.empty() && points_.front().time<snapshot.timestamp_ms-retention_ms)points_.pop_front();
    while(points_.size()>maximum_points)points_.pop_front();
    return true;
}
}
