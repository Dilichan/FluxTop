#pragma once
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

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
} // namespace fluxtop::metric_text
