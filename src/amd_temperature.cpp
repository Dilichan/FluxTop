#include "amd_temperature.hpp"
#include "fluxtop/cpu_topology_reader.hpp"
#include "fluxtop/temperatures.hpp"
#include "pawnio_reader.hpp"
#include <algorithm>
#include <intrin.h>
#include <mutex>
#include <numeric>

namespace fluxtop {
bool amd_zen_family(unsigned family) noexcept {
    return family==0x17 || family==0x19 || family==0x1a;
}
std::optional<double> decode_amd_tctl(std::uint32_t raw) noexcept {
    if(!raw || raw==UINT32_MAX)return {};
    // THM_TCON_CUR_TMP: CurTmp is in eighths of a degree; range select
    // changes the register's zero point, not the model-specific Tctl offset.
    const auto celsius=static_cast<double>(raw>>21)/8.0-((raw&(1U<<19))?49.0:0.0);
    return celsius!=0 && cpu_temperature_valid(celsius)?std::optional{celsius}:std::nullopt;
}
std::optional<unsigned> libre_amd_core_ordinal(std::wstring_view identifier,std::wstring_view name) noexcept {
    constexpr std::wstring_view prefix=L"/amdcpu/0/temperature/";
    if(!identifier.starts_with(prefix))return {};
    const auto index=identifier.substr(prefix.size());
    if(index.empty() || index.find_first_not_of(L"0123456789")!=index.npos || !name.starts_with(L"Core #"))
        return {};
    const auto ordinal=name.substr(6);
    if(ordinal.empty() || ordinal.size()>3 || ordinal.find_first_not_of(L"0123456789")!=ordinal.npos)
        return {};
    unsigned core{};
    for(const auto c:ordinal)core=core*10+static_cast<unsigned>(c-L'0');
    return core && core<=256?std::optional{core}:std::nullopt;
}
std::vector<std::size_t> amd_core_order(std::span<const unsigned> hardware_ids) {
    if(hardware_ids.size()>256 || std::ranges::any_of(hardware_ids,[](unsigned id){return id>255;}))return {};
    std::vector<std::size_t> order(hardware_ids.size());
    std::iota(order.begin(),order.end(),std::size_t{0});
    std::ranges::sort(order,{},[&](std::size_t i){return hardware_ids[i];});
    if(std::ranges::adjacent_find(order,std::equal_to{},[&](std::size_t i){return hardware_ids[i];})!=order.end())
        return {};
    return order;
}
void apply_amd_core_temperatures(CpuTopologySnapshot& topology,std::span<const std::size_t> order,
    std::span<const AmdCoreReading> readings,std::uint64_t sampled,std::uint64_t now) {
    for(auto& core:topology.cores)core.celsius.reset();
    if(order.size()==topology.cores.size() && sampled && now>=sampled && now-sampled<=20000) {
        std::array<bool,256> seen{};
        for(const auto& reading:readings) {
            if(!reading.ordinal || reading.ordinal>seen.size() || reading.ordinal>order.size())continue;
            auto& core=topology.cores[order[reading.ordinal-1]];
            const bool first=!seen[reading.ordinal-1];
            seen[reading.ordinal-1]=true;
            core.celsius=first && reading.celsius!=0 && cpu_temperature_valid(reading.celsius)
                ?std::optional{reading.celsius}:std::nullopt;
        }
        topology.sampled_tick=sampled;
    }
    finalize_core_temperatures(topology);
}

bool amd_cpu() noexcept {
    const auto vendor=read_cpu_vendor();
    return std::string_view(vendor.data(),vendor.size())=="AuthenticAMD";
}
namespace {
unsigned cpu_family() noexcept {
    int values[4]{};
    __cpuid(values,1);
    const auto signature=static_cast<unsigned>(values[0]);
    const auto base=(signature>>8)&0xf;
    return base==0xf?base+((signature>>20)&0xff):base;
}
bool single_socket() {
    DWORD bytes{};
    GetLogicalProcessorInformationEx(RelationProcessorPackage,nullptr,&bytes);
    if(!bytes || bytes>1024*1024)return false;
    std::vector<std::byte> buffer(bytes);
    auto* package=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    return GetLogicalProcessorInformationEx(RelationProcessorPackage,package,&bytes) &&
        bytes>=offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,Processor) &&
        package->Relationship==RelationProcessorPackage && package->Size==bytes;
}
std::vector<std::size_t> read_core_order(const CpuTopologySnapshot& topology) {
    int values[4]{};
    __cpuid(values,static_cast<int>(0x80000000U));
    if(static_cast<unsigned>(values[0])<0x8000001eU)return {};
    __cpuid(values,static_cast<int>(0x80000001U));
    if(!(static_cast<unsigned>(values[2])&(1U<<22)))return {};
    std::vector<unsigned> hardware_ids;
    hardware_ids.reserve(topology.cores.size());
    for(const auto& core:topology.cores) {
        if(core.logical>=64)return {};
        GROUP_AFFINITY target{},previous{};
        target.Group=static_cast<WORD>(core.group);
        target.Mask=KAFFINITY{1}<<core.logical;
        if(!SetThreadGroupAffinity(GetCurrentThread(),&target,&previous))return {};
        __cpuid(values,static_cast<int>(0x8000001eU));
        if(!SetThreadGroupAffinity(GetCurrentThread(),&previous,nullptr))return {};
        hardware_ids.push_back(static_cast<unsigned>(values[1])&0xff);
    }
    return amd_core_order(hardware_ids);
}
}
AmdCoreLayout read_amd_layout() {
    // Refresh the Windows topology after hotplug. Rebuild the hardware
    // ordinal map only when the topology changes.
    static std::mutex mutex;
    static AmdCoreLayout layout;
    static std::uint64_t last_check{};
    std::lock_guard guard(mutex);
    const auto now=GetTickCount64();
    if(last_check && now-last_check<10000)return layout;
    last_check=now;
    auto topology=read_cpu_topology();
    for(auto& core:topology.cores) {
        core.amd=true;
        core.sensor_independent_unverified=false;
    }
    const bool single=single_socket();
    if(!single || !amd_zen_family(cpu_family()))layout.libre_core_order.clear();
    else if(!layout.single_package || layout.libre_core_order.empty() || layout.topology.cores!=topology.cores) {
        layout.libre_core_order=read_core_order(topology);
        layout.order_since=now;
    }
    layout.topology=std::move(topology);
    layout.single_package=single;
    return layout;
}

CpuTemperatureCandidate read_amd_tctl(bool single_package) {
    constexpr std::uint64_t thermal_control=0x59800;
    static const bool supported=amd_zen_family(cpu_family());
    static std::mutex access;
    static CpuTemperatureCandidate cached{CpuTemperatureProvider::amd_smn,CpuTemperatureKind::control,{}};
    static PawnIoReader reader;
    static std::uint64_t next_poll{};
    std::lock_guard guard(access);
    if(!single_package || !supported) {
        reader.close();
        cached.celsius.reset();
        next_poll=0;
        return cached;
    }
    const auto now=GetTickCount64();
    if(cached.celsius && now-cached.sampled_tick>6000)cached.celsius.reset();
    if(now<next_poll)return cached;
    next_poll=now+2000;
    if(!reader.ready() && (!reader.open() || !reader.load(L"AMDFamily17.bin"))) {
        reader.close();
        next_poll=now+15000;
        return cached;
    }
    // PawnIO's SMN transaction shares the PCI index/data registers with hardware monitors.
    const Handle mutex(CreateMutexW(nullptr,FALSE,L"Global\\Access_PCI"));
    if(!mutex.valid())return cached;
    const auto wait=WaitForSingleObject(mutex.get(),0);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)return cached;
    std::uint64_t raw{};
    const bool ok=reader.read("ioctl_read_smn",thermal_control,raw);
    ReleaseMutex(mutex.get());
    if(!ok) {
        reader.close();
        next_poll=now+15000;
    }
    cached.celsius=ok?decode_amd_tctl(static_cast<std::uint32_t>(raw)):std::nullopt;
    cached.sampled_tick=now;
    return cached;
}
}
