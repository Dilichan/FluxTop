#include "fluxtop/output.hpp"
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

namespace {
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string serialize(const fluxtop::Snapshot& snapshot,unsigned top=0) {
    std::ostringstream out;
    fluxtop::write_jsonl(out,snapshot,top);
    return out.str();
}
void has(const std::string& document,const char* field) {
    require(document.find(field)!=std::string::npos,field);
}
}
int main() {
    try {
        using namespace fluxtop;
        Snapshot s;
        s.timestamp_ms=123456789;
        s.gpu_status="ready";
        s.network_status="ready";
        s.disk_io_status="partial-attribution";
        s.cpu_temperature_c=61.5;
        s.gpu_temperature_c=55.0;
        s.cpu_temperature_source="hwinfo-cpu-package";
        s.disks={{L'C',25.0},{L'D',0.0}};
        Process first;
        first.pid=100;first.name="reader";
        first.disk_read_bytes_per_second=0.0; // Proven zero, not unknown.
        first.disk_write_bytes_per_second=1234.5;
        first.disk_pressure_percent=12.0;
        first.disk_volume_bytes_per_second[2]=0.0;
        first.disk_volume_bytes_per_second[3]=1234.5;
        first.disk_volume_pressure_percent[2]=0.0;
        first.disk_volume_pressure_percent[3]=12.0;
        Process second;
        second.pid=101;second.name="unknown";
        s.processes={first,second};
        const auto original=s;
        const auto partial=serialize(s);
        has(partial,"\"schema_version\":1");
        has(partial,"\"creation_time_100ns\":null");
        has(partial,"\"working_set_bytes\":null");
        has(partial,"\"gpu_status\":\"ready\"");
        has(partial,"\"disk_measurement_quality\":\"partial\"");
        has(partial,"\"disk_io_status\":\"partial-attribution\"");
        has(partial,"\"disk_read_bytes_per_second\":0.000");
        has(partial,"\"disk_write_bytes_per_second\":1234.500");
        has(partial,"\"disk_volume_bytes_per_second\":{\"C\":0.000,\"D\":1234.500}");
        has(partial,"\"disk_volume_pressure_percent\":{\"C\":0.000,\"D\":12.000}");
        has(partial,"\"cpu_temperature_source\":\"hwinfo-cpu-package\"");
        has(partial,"\"gpu_temperature_c\":55.000");
        has(partial,"\"active_percent\":0.000");
        has(partial,"\"disk_read_bytes_per_second\":null");
        require(s.processes[0].disk_read_bytes_per_second==original.processes[0].disk_read_bytes_per_second,
            "serialization modified live metrics");
        s.disk_io_status="ready";
        has(serialize(s),"\"disk_measurement_quality\":\"complete\"");
        s.disk_io_status="administrator-required";
        s.disks.clear();
        s.cpu_temperature_c.reset();s.gpu_temperature_c.reset();
        s.cpu_temperature_source="stale-cache";
        const auto unavailable=serialize(s);
        has(unavailable,"\"disk_measurement_quality\":\"unavailable\"");
        has(unavailable,"\"logical_disks\":[]");
        has(unavailable,"\"disk_volume_bytes_per_second\":{}");
        has(unavailable,"\"cpu_temperature_c\":null");
        has(unavailable,"\"gpu_temperature_c\":null");
        has(unavailable,"\"cpu_temperature_source\":\"unavailable\"");
        s.elapsed_ms=std::numeric_limits<double>::quiet_NaN();
        s.collection_ms=std::numeric_limits<double>::infinity();
        s.cpu_temperature_c=std::numeric_limits<double>::quiet_NaN();
        s.gpu_temperature_c=std::numeric_limits<double>::infinity();
        s.processes[0].disk_read_bytes_per_second=std::numeric_limits<double>::infinity();
        s.processes[0].disk_write_bytes_per_second=-10.0;
        const auto invalid=serialize(s);
        require(invalid.find("nan")==std::string::npos && invalid.find("inf")==std::string::npos,
            "nonfinite measurements leaked into JSON");
        has(invalid,"\"disk_read_bytes_per_second\":null");
        has(invalid,"\"disk_write_bytes_per_second\":null");
        require(serialize(s)==serialize(s),"repeat serialization is unstable");
        require(serialize(s,1).find("\"pid\":101")==std::string::npos,
            "top process limit was not preserved");
        std::cout << "Extended JSONL compatibility and reliability passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}