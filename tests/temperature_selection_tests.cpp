#include "fluxtop/temperature_selection.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
using namespace fluxtop;
int main(){
    int failures=0;
    const auto check=[&](bool value,const char* label){
        if(!value){++failures;std::cerr<<"FAIL: "<<label<<'\n';}
    };
    constexpr std::uint64_t now=100000;
    auto intel=CpuTemperatureCandidate{CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,60.0,now};
    auto hwinfo=CpuTemperatureCandidate{CpuTemperatureProvider::hwinfo,CpuTemperatureKind::die,62.0,now};
    auto libre=CpuTemperatureCandidate{CpuTemperatureProvider::libre_wmi,CpuTemperatureKind::die,64.0,now};
    auto open=CpuTemperatureCandidate{CpuTemperatureProvider::open_wmi,CpuTemperatureKind::die,65.0,now};
    auto board=CpuTemperatureCandidate{CpuTemperatureProvider::hwinfo,CpuTemperatureKind::board,45.0,now};
    auto acpi=CpuTemperatureCandidate{CpuTemperatureProvider::acpi,CpuTemperatureKind::acpi,43.0,now};
    check(cpu_temperature_rank(intel.provider,intel.kind)>cpu_temperature_rank(hwinfo.provider,hwinfo.kind),"intel priority");
    check(cpu_temperature_rank(hwinfo.provider,hwinfo.kind)>cpu_temperature_rank(libre.provider,libre.kind),"hwinfo priority");
    check(cpu_temperature_rank(libre.provider,libre.kind)>cpu_temperature_rank(open.provider,open.kind),"libre priority");
    check(cpu_temperature_rank(open.provider,open.kind)>cpu_temperature_rank(board.provider,board.kind),"die always beats board");
    check(cpu_temperature_rank(board.provider,board.kind)>cpu_temperature_rank(acpi.provider,acpi.kind),"board before acpi");
    check(!cpu_temperature_candidate_valid({CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,0.0,0},now),"missing timestamp");
    check(!cpu_temperature_candidate_valid({CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,55.0,now+1},now),"future reading");
    check(!cpu_temperature_candidate_valid({CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,55.0,now-20001},now),"stale reading");
    check(!cpu_temperature_candidate_valid({CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,std::numeric_limits<double>::quiet_NaN(),now},now),"nan reading");
    check(!cpu_temperature_candidate_valid({CpuTemperatureProvider::intel_dts,CpuTemperatureKind::package,145.0,now},now),"out of range");
    CpuTemperatureSelector selector;
    std::array<CpuTemperatureCandidate,4> candidates{intel,hwinfo,board,acpi};
    check(!cpu_temperature_needs_wmi(std::span{candidates}.first(2),now),"healthy direct package or die parks WMI");
    check(cpu_temperature_needs_wmi(std::span{candidates}.subspan(2),now),"board reading keeps WMI eligible");
    check(!cpu_temperature_needs_wmi({},now,now-1999) && cpu_temperature_needs_wmi({},now,now-2000),
        "preferred provider startup delays WMI for at most two seconds");
    const std::array expired{CpuTemperatureCandidate{intel.provider,intel.kind,60.0,now-20001}};
    check(cpu_temperature_needs_wmi(expired,now),"stale preferred reading re-enables WMI");
    auto selected=selector.select(candidates,now);
    check(selected.celsius==60.0 && selected.provider==CpuTemperatureProvider::intel_dts,"native initial");
    candidates[0].celsius.reset();
    selected=selector.select(candidates,now);
    check(selected.celsius==62.0 && selected.provider==CpuTemperatureProvider::hwinfo,"immediate fallback");
    candidates[0]=intel;
    selected=selector.select(candidates,now);
    check(selected.celsius==62.0,"recover debounce first healthy sample");
    selected=selector.select(candidates,now);
    check(selected.celsius==60.0 && selected.provider==CpuTemperatureProvider::intel_dts,"recover after two samples");
    candidates[0].sampled_tick=1;
    candidates[1].sampled_tick=1;
    selected=selector.select(candidates,now);
    check(selected.celsius==45.0 && selected.kind==CpuTemperatureKind::board,"board fallback after die stale");
    candidates[2].celsius.reset();
    selected=selector.select(candidates,now);
    check(selected.celsius==43.0 && selected.kind==CpuTemperatureKind::acpi,"acpi fallback");
    candidates[3].celsius.reset();
    selected=selector.select(candidates,now);
    check(!selected.celsius,"missing measurements stay unknown");
    candidates[1]=hwinfo;
    selected=selector.select(candidates,now);
    check(selected.celsius==62.0,"restore after all sources gone");
    // Identical values do not imply identical physical sensors.
    check(cpu_temperature_kind_name(CpuTemperatureKind::board)=="cpu-socket-board","board identity");
    return failures?1:0;
}
