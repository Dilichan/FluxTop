#include "amd_temperature.hpp"
#include "fluxtop/cpu_view.hpp"
#include "fluxtop/temperatures.hpp"
#include <array>
#include <iostream>
#include <limits>

int main() {
    using namespace fluxtop;
    int failed=0;
    const auto check=[&](bool ok,const char* reason) {
        if(!ok){std::cerr<<reason<<'\n';++failed;}
    };
    check(amd_zen_family(0x17) && amd_zen_family(0x19) && amd_zen_family(0x1a) &&
        !amd_zen_family(0x16) && !amd_zen_family(0x18) && !amd_zen_family(0x1b),"scope is limited to supported AMD families");
    check(decode_amd_tctl(501U<<21)==62.625 && decode_amd_tctl((893U<<21)|(1U<<19))==62.625,
        "THM_TCON_CUR_TMP has eighth-degree precision and a range-select origin");
    check(!decode_amd_tctl(0) && !decode_amd_tctl(UINT32_MAX) && !decode_amd_tctl(1600U<<21),
        "missing, all-ones and out-of-range registers do not produce values");
    check(libre_amd_core_ordinal(L"/amdcpu/0/temperature/16",L"Core #3")==3 &&
        !libre_amd_core_ordinal(L"/amdcpu/1/temperature/16",L"Core #3") &&
        !libre_amd_core_ordinal(L"/amdcpu/0/temperature/16",L"CCD3 (Tdie)") &&
        !libre_amd_core_ordinal(L"/amdcpu/0/temperature/16",L"Core (Tctl/Tdie)") &&
        !libre_amd_core_ordinal(L"/amdcpu/0/temperature/16",L"Core #3 Maximum") &&
        !libre_amd_core_ordinal(L"/amdcpu/0/load/16",L"Core #3"),
        "only exact physical-core temperatures from the AMD CPU namespace qualify");
    check(cpu_temperature_sensor_priority(L"/amdcpu/0/temperature/16",L"CCD3 (Tdie)")==0 &&
        cpu_temperature_sensor_priority(L"/amdcpu/0/temperature/16",L"Core #3")==0 &&
        cpu_temperature_sensor_priority(L"/amdcpu/0/temperature/1",L"Core (Tdie)")==3,
        "CCD and per-core readings are never presented as the package temperature");
    const auto order=amd_core_order(std::array{8U,1U,4U});
    check(order==std::vector<std::size_t>{1,2,0} && amd_core_order(std::array{1U,1U}).empty(),
        "Libre ordinals use sorted hardware IDs with gaps and reject ambiguous IDs");
    CpuTopologySnapshot topology;
    topology.cores={{1,0,0,true,{},2,false,true},{0,0,1,true,{},2,false,true},{0,2,2,true,{},1,false,true}};
    topology.package_c=61.0;
    const std::array readings{AmdCoreReading{1,47.5},AmdCoreReading{3,55.0}};
    apply_amd_core_temperatures(topology,order,readings,1000,1001);
    check(topology.cores[0].celsius==55 && topology.cores[1].celsius==47.5 && !topology.cores[2].celsius &&
        topology.core_max_c==55 && topology.core_average_c==51.25 && topology.partial,
        "only measured cores map to Windows slots and count toward aggregates");
    const std::array invalid{AmdCoreReading{1,47.5},AmdCoreReading{1,48},AmdCoreReading{2,0},
        AmdCoreReading{3,std::numeric_limits<double>::quiet_NaN()}};
    apply_amd_core_temperatures(topology,order,invalid,1000,1001);
    check(!topology.available_cores && !topology.core_max_c,"duplicate and invalid readings are discarded");
    apply_amd_core_temperatures(topology,order,readings,1000,21001);
    check(!topology.available_cores && topology.package_c==61.0,"expiry clears only the core readings");
    check(cpu_core_label(topology.cores[0],topology.cores)==L"\u042f\u0434\u0440\u043e 1","AMD uses neutral core labels");
    check(cpu_temperature_rank(CpuTemperatureProvider::libre_wmi,CpuTemperatureKind::die)>
        cpu_temperature_rank(CpuTemperatureProvider::amd_smn,CpuTemperatureKind::control),
        "reported die temperature is preferred to AMD Tctl");
    return failed?1:0;
}
