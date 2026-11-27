#include "fluxtop/cpu_topology_reader.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string_view>

namespace fluxtop {
namespace {
std::wstring cpu_name() {
    int limits[4]{};
    __cpuid(limits,static_cast<int>(0x80000000U));
    if(static_cast<unsigned>(limits[0])<0x80000004U)return {};
    char name[49]{};
    for(unsigned leaf=0;leaf<3;++leaf) {
        int part[4]{};
        __cpuid(part,static_cast<int>(0x80000002U+leaf));
        std::memcpy(name+leaf*16,part,16);
    }
    std::wstring result;
    for(const auto c:std::string_view(name))result+=static_cast<wchar_t>(static_cast<unsigned char>(c));
    result.erase(0,result.find_first_not_of(L' '));
    return result;
}
}

std::array<char,12> read_cpu_vendor() noexcept {
    int data[4]{};
    __cpuid(data,0);
    std::array<char,12> vendor{};
    std::memcpy(vendor.data(),data+1,4);
    std::memcpy(vendor.data()+4,data+3,4);
    std::memcpy(vendor.data()+8,data+2,4);
    return vendor;
}

// A missing core is represented by nullopt. Never include it in aggregates.
void finalize_core_temperatures(CpuTopologySnapshot& sample) noexcept {
    sample.physical_cores=static_cast<unsigned>(sample.cores.size());
    sample.available_cores=0;
    sample.core_max_c.reset();
    sample.core_average_c.reset();
    double sum=0.0;
    for(const auto& core:sample.cores){
        if(!core.celsius || !std::isfinite(*core.celsius) ||
            *core.celsius < -10.0 || *core.celsius>125.0)continue;
        ++sample.available_cores;
        sum+=*core.celsius;
        if(!sample.core_max_c || *sample.core_max_c<*core.celsius)
            sample.core_max_c=*core.celsius;
    }
    if(sample.available_cores)
        sample.core_average_c=sum/static_cast<double>(sample.available_cores);
    sample.partial=sample.physical_cores==0 ||
        sample.available_cores<sample.physical_cores || !sample.package_c;
}
CpuTopologySnapshot read_cpu_topology() {
    CpuTopologySnapshot result;
    result.cpu_name=cpu_name();
    result.sampled_tick=GetTickCount64();
    result.status="topology-unavailable";
    DWORD bytes=0;
    GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&bytes);
    if(!bytes || bytes>1024*1024)return result;
    std::vector<std::byte> buffer(bytes);
    if(!GetLogicalProcessorInformationEx(RelationProcessorCore,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),&bytes) || bytes>buffer.size())return result;

    constexpr auto header_size=offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,Processor);
    constexpr auto masks_offset=header_size+offsetof(PROCESSOR_RELATIONSHIP,GroupMask);
    std::vector<const PROCESSOR_RELATIONSHIP*> cores;
    BYTE highest_class=0;
    for(std::size_t pos=0;pos<bytes;) {
        if(bytes-pos<header_size)return result;
        const auto* item=reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()+pos);
        if(item->Size<header_size || item->Size>bytes-pos)return result;
        if(item->Relationship==RelationProcessorCore) {
            if(item->Size<masks_offset)return result;
            const auto& core=item->Processor;
            if(!core.GroupCount || core.GroupCount>(item->Size-masks_offset)/sizeof(GROUP_AFFINITY))return result;
            cores.push_back(&core);
            highest_class=std::max(highest_class,core.EfficiencyClass);
        }
        pos+=item->Size;
    }
    for(const auto* core:cores) {
        const auto id=static_cast<unsigned>(result.cores.size());
        const bool performance=core->EfficiencyClass==highest_class;
        const auto first=result.logical_members.size();
        for(WORD index=0;index<core->GroupCount;++index) {
            const auto& group=core->GroupMask[index];
            for(auto mask=group.Mask;mask;mask&=mask-1) {
                const auto logical=static_cast<unsigned>(std::countr_zero(mask));
                result.logical_members.push_back({group.Group,logical,id,performance});
            }
        }
        if(first==result.logical_members.size())continue;
        const auto& representative=result.logical_members[first];
        result.cores.push_back({representative.group,representative.number,id,performance,
            std::nullopt,static_cast<unsigned>(result.logical_members.size()-first),true});
    }
    result.physical_cores=static_cast<unsigned>(result.cores.size());
    result.logical_processors=static_cast<unsigned>(result.logical_members.size());
    result.partial=!result.cores.empty();
    if(!result.cores.empty())result.status="topology-ready";
    return result;
}
} // namespace fluxtop
