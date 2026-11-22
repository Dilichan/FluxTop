#include "fluxtop/temperatures.hpp"
#include <iostream>
#include <limits>

int main() {
    using fluxtop::cpu_temperature_sensor_priority;
    int failed=0;
    auto check=[&](bool condition,const char* message) {
        if (!condition) { ++failed; std::cerr << "FAIL: " << message << '\n'; }
    };
    check(cpu_temperature_sensor_priority(L"/intelcpu/0/temperature/0",L"CPU Package")==5,"Intel package recognized");
    check(cpu_temperature_sensor_priority(L"/amdcpu/0/temperature/0",L"CPU (Tctl/Tdie)")==4,"AMD die recognized");
    check(cpu_temperature_sensor_priority(L"/cpu/0/temperature/0",L"CPU Die")==3,"generic die recognized");
    check(cpu_temperature_sensor_priority(L"/intelcpu/0/temperature/1",L"Core Average")==2,"core average fallback");
    check(cpu_temperature_sensor_priority(L"/intelcpu/0/temperature/1",L"Core #1")==1,"core fallback");
    check(cpu_temperature_sensor_priority(L"/mainboard/0/temperature/0",L"CPU Package")==0,"board sensor must not masquerade as package");
    check(fluxtop::motherboard_cpu_sensor_priority(L"/mainboard/0/temperature/0",L"CPU")==2,
        "explicit board CPU sensor is accepted as a board temperature");
    check(fluxtop::motherboard_cpu_sensor_priority(L"/mainboard/0/temperature/1",L"CPU Socket")==4,
        "board socket sensor accepted");
    check(fluxtop::motherboard_cpu_sensor_priority(L"/lpc/nct67xx/temperature/1",L"CPU")==2,
        "generic Super I/O CPU sensor accepted");
    check(fluxtop::motherboard_cpu_sensor_priority(L"/mainboard/0/temperature/2",L"VRM")==0,
        "VRM sensor is not a CPU zone");
    check(fluxtop::motherboard_cpu_sensor_priority(L"/gpu-amd/0/temperature/1",L"CPU")==0,
        "GPU label cannot masquerade as motherboard sensor");
    check(cpu_temperature_sensor_priority(L"/gpu-amd/0/temperature/0",L"CPU Package")==0,"GPU must not masquerade as CPU");
    check(cpu_temperature_sensor_priority(L"/intelcpu/0/load/0",L"CPU Total")==0,"load sensor must not be CPU temperature");
    check(cpu_temperature_sensor_priority(L"/intelcpu/0/temperature/0",L"TjMax")==0,"TjMax is not a measured package temperature");
    using namespace fluxtop;
    check(hwinfo_cpu_sensor_priority("cpu [#0] intel","cpu package")==5,
        "HWiNFO CPU package identified");
    check(hwinfo_cpu_sensor_priority("cpu [#0] amd","core average")==2,
        "HWiNFO core average fallback identified");
    check(hwinfo_cpu_sensor_priority("mainboard cpu", "cpu package")==0,
        "board package is never a CPU die");
    check(hwinfo_cpu_sensor_priority("gpu nvidia","cpu package")==0,
        "GPU group cannot impersonate CPU");
    check(hwinfo_cpu_sensor_priority("cpu intel","tjmax")==0,
        "TjMax is limit, not measurement");
    check(hwinfo_board_sensor_priority("nuvoton super i/o", "cpu socket")==4,
        "board CPU socket fallback identified");
    check(hwinfo_board_sensor_priority("nuvoton super i/o", "vrm")==0,
        "VRM excluded from CPU board readings");
    check(hwinfo_board_sensor_priority("mainboard", "pch")==0,
        "PCH excluded from CPU board readings");
    check(acpi_cpu_zone_candidate(L"ACPI\\ThermalZone\\CPU0"),
        "explicit ACPI CPU zone recognized");
    check(!acpi_cpu_zone_candidate(L"ACPI\\ThermalZone\\TZ00") &&
        !acpi_cpu_zone_candidate(L"ACPI\\ThermalZone\\VRM"),
        "unknown and VRM ACPI zones excluded");
    check(cpu_temperature_valid(0.0) && cpu_temperature_valid(100.0),
        "measured CPU temperature zero and 100 are valid");
    check(!cpu_temperature_valid(std::numeric_limits<double>::quiet_NaN()) &&
        !cpu_temperature_valid(std::numeric_limits<double>::infinity()) &&
        !cpu_temperature_valid(140.0),
        "non-finite and impossible CPU readings rejected");
    check(cpu_temperature_fresh(90,100,30) &&
        !cpu_temperature_fresh(60,100,30) &&
        !cpu_temperature_fresh(101,100,30) &&
        !cpu_temperature_fresh(0,100,30),
        "freshness rejects stale, future, and missing HWiNFO samples");
    check(cpu_temperature_should_replace(false,false,true) &&
        cpu_temperature_should_replace(true,true,false) &&
        !cpu_temperature_should_replace(true,false,true) &&
        !cpu_temperature_should_replace(true,true,true),
        "CPU die has priority over board, and existing valid source stays stable");
    return failed ? 1 : 0;
}