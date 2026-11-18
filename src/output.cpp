#include "fluxtop/output.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace fluxtop {
std::string json_escape(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : input) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 32) {
                result += "\\u00";
                result += hex[c >> 4];
                result += hex[c & 15];
            } else result += static_cast<char>(c);
        }
    }
    result += '"';
    return result;
}

namespace {
template<class T> void nullable(std::ostream& out, const std::optional<T>& value) {
    if (value) out << *value;
    else out << "null";
}
template<> void nullable(std::ostream& out, const std::optional<double>& value) {
    if (value && std::isfinite(*value)) out << *value;
    else out << "null";
}
std::string number(std::optional<double> value) {
    if (!value) return "-";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << *value;
    return out.str();
}
std::string mib(std::optional<std::uint64_t> value) {
    return number(value ? std::optional<double>(*value / 1048576.0) : std::nullopt);
}
std::size_t row_count(const Snapshot& s, unsigned top) {
    return top ? std::min(s.processes.size(), static_cast<std::size_t>(top)) : s.processes.size();
}
} // namespace

void write_jsonl(std::ostream& out, const Snapshot& s, unsigned top) {
    out << std::fixed << std::setprecision(3)
        << "{\"schema_version\":1,\"timestamp_ms\":" << s.timestamp_ms
        << ",\"elapsed_ms\":" << s.elapsed_ms << ",\"collection_ms\":" << s.collection_ms
        << ",\"logical_processors\":" << s.logical_processors
        << ",\"process_count\":" << s.processes.size() << ",\"inaccessible\":" << s.inaccessible
        << ",\"gpu_status\":" << json_escape(s.gpu_status)
        << ",\"self\":{\"pid\":" << s.self_pid << ",\"cpu_percent\":";
    nullable(out, s.self_cpu_percent);
    out << ",\"working_set_bytes\":"; nullable(out, s.self_working_set);
    out << ",\"private_bytes\":"; nullable(out, s.self_private_bytes);
    out << ",\"peak_working_set_bytes\":"; nullable(out, s.self_peak_working_set);
    out << ",\"handles\":"; nullable(out, s.self_handles);
    out << "},\"processes\":[";
    for (std::size_t i = 0; i < row_count(s, top); ++i) {
        const auto& p = s.processes[i];
        if (i) out << ',';
        out << "{\"pid\":" << p.pid << ",\"name\":" << json_escape(p.name) << ",\"creation_time_100ns\":";
        // Decimal string preserves 64-bit FILETIME precision in JavaScript consumers.
        if (p.creation_time) out << '"' << *p.creation_time << '"'; else out << "null";
        out << ",\"cpu_percent\":"; nullable(out, p.cpu_percent);
        out << ",\"working_set_bytes\":"; nullable(out, p.working_set);
        out << ",\"private_bytes\":"; nullable(out, p.private_bytes);
        out << ",\"gpu_percent\":"; nullable(out, p.gpu_percent);
        out << ",\"dedicated_gpu_bytes\":"; nullable(out, p.dedicated_gpu_bytes);
        out << '}';
    }
    out << "]}\n";
}

void write_table(std::ostream& out, const Snapshot& s, unsigned top) {
    out << "\nFluxTop | " << s.processes.size() << " processes | " << s.inaccessible
        << " inaccessible | GPU " << s.gpu_status << " | collect " << number(s.collection_ms) << " ms\n"
        << "Self: CPU " << number(s.self_cpu_percent) << "% | WS " << mib(s.self_working_set)
        << " MiB | private " << mib(s.self_private_bytes) << " MiB | handles ";
    if (s.self_handles) out << *s.self_handles; else out << '-';
    out << "\n" << std::right << std::setw(8) << "PID" << std::setw(9) << "CPU %"
        << std::setw(12) << "WS MiB" << std::setw(12) << "Private MiB"
        << std::setw(9) << "GPU %" << std::setw(12) << "GPU MiB" << "  Process\n";
    for (std::size_t i = 0; i < row_count(s, top); ++i) {
        const auto& p = s.processes[i];
        auto name = p.name;
        for (auto& c : name) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
        out << std::setw(8) << p.pid << std::setw(9) << number(p.cpu_percent)
            << std::setw(12) << mib(p.working_set) << std::setw(12) << mib(p.private_bytes)
            << std::setw(9) << number(p.gpu_percent) << std::setw(12) << mib(p.dedicated_gpu_bytes)
            << "  " << name << '\n';
    }
}
} // namespace fluxtop
