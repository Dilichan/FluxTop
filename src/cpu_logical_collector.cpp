#include "fluxtop/cpu_logical_collector.hpp"
#include <pdhmsg.h>
#include <algorithm>
#include <string>
#include <utility>

namespace fluxtop {
CpuLogicalCollector::~CpuLogicalCollector(){reset();}
void CpuLogicalCollector::reset() noexcept {
    if(query_)PdhCloseQuery(query_);
    query_=nullptr;
    counters_.clear();
    have_baseline_=false;
    previous_tick_=0;
}
bool CpuLogicalCollector::configure(const std::vector<IntelLogicalProcessor>& topology) {
    reset();
    if(topology.empty())return false;
    if(PdhOpenQueryW(nullptr,0,&query_)!=ERROR_SUCCESS){reset();return false;}
    counters_.reserve(topology.size());
    for(const auto& cpu:topology){
        // Processor Information instances use (processor group, group-local CPU number).
        // PdhAddEnglishCounterW avoids localized counter display names.
        std::wstring path=L"\\Processor Information("+
            std::to_wstring(cpu.group)+L","+std::to_wstring(cpu.number)+
            L")\\% Processor Time";
        PDH_HCOUNTER handle{};
        if(PdhAddEnglishCounterW(query_,path.c_str(),0,&handle)!=ERROR_SUCCESS)
            handle=nullptr;
        counters_.push_back({cpu.group,cpu.number,cpu.core_id,cpu.performance,handle});
    }
    return true;
}
void CpuLogicalCollector::collect(std::vector<CpuLogicalUsage>& values,std::string& status,
    const std::vector<IntelLogicalProcessor>& topology) {
    values.clear();
    if(topology.empty()){reset();status="topology-unavailable";return;}
    bool changed=counters_.size()!=topology.size();
    if(!changed)
        for(std::size_t i=0;i<topology.size();++i)
            if(counters_[i].group!=topology[i].group ||
               counters_[i].number!=topology[i].number ||
               counters_[i].core_id!=topology[i].core_id ||
               counters_[i].performance!=topology[i].performance){changed=true;break;}
    if(changed && !configure(topology)){status="counter-unavailable";return;}
    values.reserve(topology.size());
    for(const auto& cpu:topology)
        values.push_back({cpu.group,cpu.number,cpu.core_id,cpu.performance,std::nullopt});
    const auto now=GetTickCount64();
    if(previous_tick_ && (now<previous_tick_ || now-previous_tick_>30000))have_baseline_=false;
    if(!query_ || PdhCollectQueryData(query_)!=ERROR_SUCCESS){
        have_baseline_=false;
        previous_tick_=now;
        status="counter-unavailable";
        return;
    }
    previous_tick_=now;
    if(!have_baseline_){
        have_baseline_=true;
        status="baseline";
        return;
    }
    unsigned available=0;
    for(std::size_t i=0;i<counters_.size();++i){
        if(!counters_[i].handle)continue;
        PDH_FMT_COUNTERVALUE value{};
        DWORD counter_type{};
        if(PdhGetFormattedCounterValue(counters_[i].handle,PDH_FMT_DOUBLE,&counter_type,&value)!=ERROR_SUCCESS)
            continue;
        if(value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA)
            continue;
        values[i].percent=cpu_logical_percent(value.doubleValue);
        if(values[i].percent)++available;
    }
    status=available==values.size()?"ready":available?"partial":"counter-unavailable";
}
} // namespace fluxtop