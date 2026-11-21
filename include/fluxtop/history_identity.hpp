#pragma once
#include "fluxtop/model.hpp"
#include <cmath>
#include <string>

namespace fluxtop {
// Stable executable identity; unknown images use verified process lifetime.
// Separate unrelated same-name processes without altering normal path grouping.
inline std::string history_identity(const Process& p) {
    if (p.exe_path && !p.exe_path->empty()) return "path:" + *p.exe_path;
    return "pid:" + std::to_string(p.pid) + ":"
        + (p.creation_time ? std::to_string(*p.creation_time) : std::string("unknown"));
}
// A real measured zero is valid; no measured metrics means unknown AutoScore.
inline bool history_score_available(const Process& p) {
    const auto valid=[](std::optional<double> v) {
        return v && std::isfinite(*v) && *v>=0.0;
    };
    return valid(p.cpu_percent) || p.working_set.has_value()
        || valid(p.gpu_percent) || p.dedicated_gpu_bytes.has_value()
        || valid(p.rx_bytes_per_second) || valid(p.tx_bytes_per_second)
        || valid(p.disk_read_bytes_per_second) || valid(p.disk_write_bytes_per_second);
}
} // namespace fluxtop