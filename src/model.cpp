#include "fluxtop/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace fluxtop {

std::vector<Process> CpuTracker::update(const std::vector<RawProcess>& raw, unsigned processors) {
    std::unordered_map<std::uint32_t, Baseline> next;
    next.reserve(raw.size());
    std::vector<Process> rows;
    rows.reserve(raw.size());
    for (const auto& sample : raw) {
        Process row;
        static_cast<RawProcess&>(row) = sample;
        if (sample.creation_time && sample.cpu_ticks) {
            const auto found = previous_.find(sample.pid);
            if (found != previous_.end() && found->second.creation == *sample.creation_time) {
                const auto& before = found->second;
                row.has_baseline = true;
                if (processors && sample.sampled_ns > before.sampled_ns && *sample.cpu_ticks >= before.ticks) {
                    const double elapsed = static_cast<double>(sample.sampled_ns - before.sampled_ns);
                    const double ticks = static_cast<double>(*sample.cpu_ticks - before.ticks);
                    row.cpu_percent = std::clamp(ticks * 100.0 * 100.0 / elapsed / processors, 0.0, 100.0);
                }
            }
            next.emplace(sample.pid, Baseline{*sample.creation_time, *sample.cpu_ticks, sample.sampled_ns});
        }
        rows.push_back(std::move(row));
    }
    previous_ = std::move(next);
    return rows;
}

void SpikeTracker::update(std::vector<Process>& rows) {
    std::unordered_map<std::uint32_t, Process> next;
    next.reserve(rows.size());
    for (auto& row : rows) {
        row.spike_score = 0;
        const auto found = previous_.find(row.pid);
        if (row.creation_time && found != previous_.end() && found->second.creation_time == row.creation_time) {
            const auto& before = found->second;
            const auto growth = [](auto now, auto old, double scale) {
                return now && old && *now > *old ? (static_cast<double>(*now) - static_cast<double>(*old)) / scale : 0.0;
            };
            row.spike_score = std::max({growth(row.cpu_percent, before.cpu_percent, 5.0),
                growth(row.gpu_percent, before.gpu_percent, 5.0),
                growth(row.working_set, before.working_set, 64.0 * 1048576),
                growth(row.dedicated_gpu_bytes, before.dedicated_gpu_bytes, 64.0 * 1048576),
                growth(row.rx_bytes_per_second, before.rx_bytes_per_second, 64.0 * 1024),
                growth(row.tx_bytes_per_second, before.tx_bytes_per_second, 64.0 * 1024)});
        }
        next.emplace(row.pid, row);
    }
    previous_ = std::move(next);
}

void sort_processes(std::vector<Process>& rows, Sort sort) {
    const auto value = [sort](const Process& row) -> std::optional<double> {
        switch (sort) {
        case Sort::cpu: return row.cpu_percent;
        case Sort::ram: return row.working_set ? std::optional<double>(static_cast<double>(*row.working_set)) : std::nullopt;
        case Sort::gpu: return row.gpu_percent;
        case Sort::vram: return row.dedicated_gpu_bytes ? std::optional<double>(static_cast<double>(*row.dedicated_gpu_bytes)) : std::nullopt;
        case Sort::automatic: return row.spike_score >= 1.0 ? row.spike_score : 0.0;
        case Sort::rx: return row.rx_bytes_per_second;
        case Sort::tx: return row.tx_bytes_per_second;
        case Sort::network:
            if (row.rx_bytes_per_second && row.tx_bytes_per_second)
                return *row.rx_bytes_per_second + *row.tx_bytes_per_second;
            return std::nullopt;
        }
        return std::nullopt;
    };
    std::sort(rows.begin(), rows.end(), [&](const Process& a, const Process& b) {
        const auto av = value(a);
        const auto bv = value(b);
        if (av.has_value() != bv.has_value()) return av.has_value();
        if (av && *av != *bv) return *av > *bv;
        if (sort == Sort::automatic && a.cpu_percent != b.cpu_percent) {
            if (a.cpu_percent.has_value() != b.cpu_percent.has_value()) return a.cpu_percent.has_value();
            return *a.cpu_percent > *b.cpu_percent;
        }
        return a.pid < b.pid;
    });
}

std::optional<std::uint32_t> gpu_instance_pid(const std::wstring& instance) {
    constexpr std::wstring_view prefix = L"pid_";
    if (!instance.starts_with(prefix)) return std::nullopt;
    std::uint64_t result = 0;
    std::size_t i = prefix.size();
    const auto start = i;
    for (; i < instance.size() && instance[i] >= L'0' && instance[i] <= L'9'; ++i) {
        result = result * 10 + (instance[i] - L'0');
        if (result > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    }
    if (i == start || i == instance.size() || instance[i] != L'_') return std::nullopt;
    return static_cast<std::uint32_t>(result);
}

} // namespace fluxtop
