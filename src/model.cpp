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

void sort_processes(std::vector<Process>& rows, Sort sort) {
    const auto value = [sort](const Process& row) -> std::optional<double> {
        switch (sort) {
        case Sort::cpu: return row.cpu_percent;
        case Sort::ram: return row.working_set ? std::optional<double>(static_cast<double>(*row.working_set)) : std::nullopt;
        case Sort::gpu: return row.gpu_percent;
        case Sort::vram: return row.dedicated_gpu_bytes ? std::optional<double>(static_cast<double>(*row.dedicated_gpu_bytes)) : std::nullopt;
        }
        return std::nullopt;
    };
    std::sort(rows.begin(), rows.end(), [&](const Process& a, const Process& b) {
        const auto av = value(a);
        const auto bv = value(b);
        if (av.has_value() != bv.has_value()) return av.has_value();
        if (av && *av != *bv) return *av > *bv;
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
