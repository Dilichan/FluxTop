#include "fluxtop/temperatures.hpp"
#include <iostream>

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
    return failed ? 1 : 0;
}