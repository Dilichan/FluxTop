#include "fluxtop/temperatures.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace fluxtop {
namespace {
#pragma pack(push,1)
struct Header {
    char signature[4];
    std::uint32_t version, revision;
    std::int64_t poll_time;
    std::uint32_t sensor_offset, sensor_stride, sensor_count;
    std::uint32_t reading_offset, reading_stride, reading_count, poll_period;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 48);
std::string ascii_lower(const char* ptr, std::size_t limit) {
    std::size_t len=0;
    while (len<limit && ptr[len]) ++len;
    std::string result(ptr,ptr+len);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
bool within(std::size_t offset, std::size_t stride, std::size_t count, std::size_t end) {
    return offset <= end && stride && count <= (end-offset)/stride;
}
} // namespace
HardwareTemperatures read_hwinfo_temperatures() {
    HardwareTemperatures result;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!mapping) return result;
    const auto* data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    if (!data) { CloseHandle(mapping); return result; }
    do {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(data,&info,sizeof(info)) || info.RegionSize < sizeof(Header)) break;
        const std::size_t total = info.RegionSize;
        Header header{};
        std::memcpy(&header,data,sizeof(header));
        if (std::memcmp(header.signature,"HWiS",4) || header.version == 0 || header.version > 2) break;
        const std::time_t now = std::time(nullptr);
        if (header.poll_time <= 0 || std::abs(static_cast<double>(now) - static_cast<double>(header.poll_time)) > 30) break;
        if (header.sensor_stride < 264 || header.reading_stride < 292
            || !within(header.sensor_offset,header.sensor_stride,header.sensor_count,total)
            || !within(header.reading_offset,header.reading_stride,header.reading_count,total)) break;
        int cpu_priority=0, gpu_priority=0;
        for (std::size_t index=0;index<header.reading_count;++index) {
            const auto* read = data + header.reading_offset + index*header.reading_stride;
            std::uint32_t type=0, sensor=0;
            std::memcpy(&type,read,4); std::memcpy(&sensor,read+4,4);
            if (type!=1 || sensor>=header.sensor_count) continue;
            const auto* source = data + header.sensor_offset + static_cast<std::size_t>(sensor)*header.sensor_stride;
            const auto group = ascii_lower(reinterpret_cast<const char*>(source+8),128);
            const auto label = ascii_lower(reinterpret_cast<const char*>(read+12),128);
            double temperature=0;
            std::memcpy(&temperature,read+284,sizeof(temperature));
            if (!std::isfinite(temperature) || temperature < 10 || temperature > 115) continue;
            const bool cpu = group.find("cpu")!=std::string::npos || group.find("processor")!=std::string::npos;
            const bool gpu = group.find("gpu")!=std::string::npos;
            const int cp = label.find("cpu package")!=std::string::npos ? 3
                : label.find("tctl/tdie")!=std::string::npos ? 3
                : label.find("cpu die")!=std::string::npos ? 2 : 0;
            const int gp = label.find("gpu temperature")!=std::string::npos ? 3
                : label.find("gpu core")!=std::string::npos && label.find("temperature")!=std::string::npos ? 2 : 0;
            if (cpu && cp>cpu_priority) { result.cpu_c=temperature; cpu_priority=cp; }
            if (gpu && gp>gpu_priority) { result.gpu_c=temperature; gpu_priority=gp; }
        }
    } while(false);
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return result;
}
} // namespace fluxtop