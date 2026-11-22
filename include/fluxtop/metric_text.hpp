#pragma once
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace fluxtop::metric_text {
// Shared, locale-independent UI format: the overlay and Statistics must not
// disagree on units, precision, or the representation of missing readings.
inline std::wstring decimal(double value,int precision=1) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream<<std::fixed<<std::setprecision(precision)<<value;
    return stream.str();
}
inline std::wstring percent(std::optional<double> value) {
    return value && std::isfinite(*value)?decimal(*value)+L"%":L"—";
}
inline std::wstring memory(std::optional<std::uint64_t> bytes) {
    if(!bytes)return L"—";
    const double mib=static_cast<double>(*bytes)/1048576.0;
    return mib>=1024.0?decimal(mib/1024.0)+L" GiB":decimal(mib)+L" MiB";
}
// Byte/s -> decimal Mb/s. The unit occurs once per Rx/Tx pair.
inline std::wstring network_rate(std::optional<double> rx,std::optional<double> tx) {
    const auto one=[](std::optional<double> bytes)->std::wstring {
        return bytes && std::isfinite(*bytes)?decimal(*bytes*8.0/1000000.0):L"—";
    };
    return one(rx)+L" / "+one(tx)+L" Mbps";
}
// System-wide PDH activity (percent) is not physical process throughput.
inline std::wstring disk_activity(std::optional<double> value) {
    if (!value || !std::isfinite(*value) || *value < 0.0) return L"—";
    return decimal(*value)+L"%";
}
// ETW-confirmed physical disk bytes/second. Partial attribution is a lower bound.
inline std::wstring disk_rate(std::optional<double> bytes_per_second,bool partial=false) {
    if (!bytes_per_second || !std::isfinite(*bytes_per_second) || *bytes_per_second<0.0)
        return L"—";
    const double rate=*bytes_per_second;
    const std::wstring prefix=partial && rate>0.0?L"≥":L"";
    if(rate==0.0)return L"0 Б/с";
    if(rate<1024.0)return prefix+decimal(rate)+L" Б/с";
    if(rate<1048576.0)return prefix+decimal(rate/1024.0)+L" КиБ/с";
    return prefix+decimal(rate/1048576.0)+L" МиБ/с";
}
inline std::wstring disk_name(wchar_t letter) {
    return letter>=L'A' && letter<=L'Z'?L"Disk "+std::wstring(1,letter):L"Disk";
}
// Provenance is explicit; absent sensor data must never be presented as CPU die.
inline std::wstring cpu_sensor_name(std::string_view source,bool board) {
    if(source=="unavailable" || source.empty())return L"Источник неизвестен";
    if(source.find("acpi")!=std::string_view::npos)return L"ACPI";
    return board?L"датчик платы":L"CPU Package/Die";
}
inline std::wstring temperature(std::optional<double> celsius) {
    return celsius && std::isfinite(*celsius)?decimal(*celsius)+L" °C":L"—";
}
} // namespace fluxtop::metric_text
