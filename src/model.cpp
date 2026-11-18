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

namespace {
// Normalized sustained load + rising-edge burst; thresholds are deliberately conservative.
constexpr double mib = 1048576.0;
constexpr double kib = 1024.0;
constexpr std::array<double,6> sustained_units{10.0,256.0*mib,10.0,256.0*mib,64.0*kib,64.0*kib};
constexpr std::array<double,6> burst_units{5.0,64.0*mib,5.0,64.0*mib,64.0*kib,64.0*kib};
constexpr std::array<unsigned,6> visible_bits{1U,2U,4U,8U,16U,16U};
std::array<std::optional<double>,6> metric_values(const Process& p) {
    return {p.cpu_percent,
        p.working_set ? std::optional<double>(static_cast<double>(*p.working_set)) : std::nullopt,
        p.gpu_percent,
        p.dedicated_gpu_bytes ? std::optional<double>(static_cast<double>(*p.dedicated_gpu_bytes)) : std::nullopt,
        p.rx_bytes_per_second,p.tx_bytes_per_second};
}
double nonnegative(std::optional<double> value) {
    return value && std::isfinite(*value) && *value > 0 ? *value : 0.0;
}
} // namespace

void SpikeTracker::update(std::vector<Process>& rows) {
    std::unordered_map<std::uint32_t, Process> next;
    next.reserve(rows.size());
    for (auto& row : rows) {
        row.spike_score = 0;
        row.auto_scores.fill(0);
        row.burst_scores.fill(0);
        const auto found = previous_.find(row.pid);
        const bool same_process = row.creation_time && found != previous_.end()
            && found->second.creation_time == row.creation_time;
        const auto current = metric_values(row);
        const auto previous = same_process ? metric_values(found->second)
            : std::array<std::optional<double>,6>{};
        for (std::size_t i = 0; i < current.size(); ++i) {
            const double now = nonnegative(current[i]);
            const double before = nonnegative(previous[i]);
            const double burst = same_process && current[i] && previous[i] && now > before
                ? (now-before) / burst_units[i] : 0.0;
            row.spike_score = std::max(row.spike_score,burst);
            row.burst_scores[i] = burst;
            // Sustained load must rank even when the derivative is zero.
            const double steady = now >= sustained_units[i]
                ? std::log2(1.0 + now / sustained_units[i]) : 0.0;
            // Decay previous priority only while the process is still significantly active.
            const double retained = same_process && steady >= 1.0
                ? found->second.auto_scores[i] * 0.65 : 0.0;
            row.auto_scores[i] = std::max({steady,burst,retained});
        }
        next.emplace(row.pid,row);
    }
    previous_ = std::move(next);
}

AutoRank auto_rank(const Process& row, unsigned visible_metrics) {
    AutoRank result{};
    for (std::size_t i = 0; i < row.auto_scores.size(); ++i) {
        if (!(visible_metrics & visible_bits[i])) continue;
        const double score = row.auto_scores[i];
        if (std::isfinite(score) && score >= 1.0 && score > result.score) {
            result.score = score;
            result.metric = static_cast<AutoMetric>(i+1);
            // Orange only when the burst actually determines the rank.
            result.burst = row.burst_scores[i] >= 1.0 && row.burst_scores[i] >= score - 1e-9;
        }
    }
    return result;
}

void sort_processes(std::vector<Process>& rows, Sort sort, unsigned visible_metrics) {
    const auto value = [sort,visible_metrics](const Process& row) -> std::optional<double> {
        switch (sort) {
        case Sort::cpu: return row.cpu_percent;
        case Sort::ram: return row.working_set ? std::optional<double>(static_cast<double>(*row.working_set)) : std::nullopt;
        case Sort::gpu: return row.gpu_percent;
        case Sort::vram: return row.dedicated_gpu_bytes ? std::optional<double>(static_cast<double>(*row.dedicated_gpu_bytes)) : std::nullopt;
        case Sort::automatic: return auto_rank(row, visible_metrics).score;
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
