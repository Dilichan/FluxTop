#include "fluxtop/metric_text.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
namespace {
void check(bool condition,const char* description) {
    if(!condition)throw std::runtime_error(description);
}
}
int main() {
    using namespace fluxtop::metric_text;
    try {
        check(disk_rate(std::nullopt)==L"—","unknown ETW rate");
        check(disk_rate(0.0)==L"0 Б/с","confirmed zero");
        check(disk_rate(1023.0)==L"1023.0 Б/с","byte units");
        check(disk_rate(1024.0)==L"1.0 КиБ/с","kibibyte units");
        check(disk_rate(1048576.0)==L"1.0 МиБ/с","mebibyte units");
        check(disk_rate(2048.0,true)==L"≥2.0 КиБ/с","partial ETW lower bound");
        check(disk_rate(0.0,true)==L"0 Б/с","zero not marked as lower bound");
        check(disk_rate(std::numeric_limits<double>::quiet_NaN())==L"—","NaN is unknown");
        check(disk_rate(std::numeric_limits<double>::infinity())==L"—","infinity is unknown");
        check(disk_rate(-5.0)==L"—","invalid negative rate");
        check(disk_activity(std::nullopt)==L"—","missing PDH activity");
        check(disk_activity(0.0)==L"0.0%","confirmed idle PDH activity");
        check(disk_activity(12.5)==L"12.5%","PDH activity is percent, not throughput");
        check(disk_name(L'C')==L"Disk C","canonical disk label");
        check(disk_name(L'D')==L"Disk D","second canonical disk label");
        check(temperature(std::nullopt)==L"—","unknown temperature lacks fake unit");
        check(temperature(0.0)==L"0.0 °C","valid zero Celsius");
        check(temperature(std::numeric_limits<double>::infinity())==L"—","invalid temperature");
        check(cpu_sensor_name("acpi-board",true)==L"ACPI","ACPI source");
        check(cpu_sensor_name("hwinfo-board",true)==L"датчик платы","board source");
        check(cpu_sensor_name("hwinfo-die",false)==L"CPU Package/Die","die source");
        check(cpu_sensor_name("unavailable",false)==L"Источник неизвестен","unavailable source");
        std::cout<<"v44 formatting checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}