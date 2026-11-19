#include "fluxtop/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace fluxtop {

std::vector<Process> CpuTracker::update(std::vector<RawProcess> raw, unsigned processors) {
    std::unordered_map<std::uint32_t, Baseline> next;
    next.reserve(raw.size());
    std::vector<Process> rows;
    rows.reserve(raw.size());
    for (auto& sample : raw) {
        Process row;
        static_cast<RawProcess&>(row) = std::move(sample);
        if (row.creation_time && row.cpu_ticks) {
            const auto found = previous_.find(row.pid);
            if (found != previous_.end() && found->second.creation == *row.creation_time) {
                const auto& before = found->second;
                row.has_baseline = true;
                if (processors && row.sampled_ns > before.sampled_ns && *row.cpu_ticks >= before.ticks) {
                    const double elapsed = static_cast<double>(row.sampled_ns - before.sampled_ns);
                    const double ticks = static_cast<double>(*row.cpu_ticks - before.ticks);
                    row.cpu_percent = std::clamp(ticks * 100.0 * 100.0 / elapsed / processors, 0.0, 100.0);
                }
            }
            next.emplace(row.pid, Baseline{*row.creation_time, *row.cpu_ticks, row.sampled_ns});
        }
        rows.push_back(std::move(row));
    }
    previous_ = std::move(next);
    return rows;
}

namespace {
// FluxTop AutoScore 0.2.1 scientific heuristic:
// capacity normalization + dominant resource + bounded complementary score
// + EWMA for retained pressure + separate short-lived rising-edge detector.
// These weights are engineering choices to be calibrated, not research constants.
constexpr double mib = 1048576.0;
constexpr double gib = 1073741824.0;
// Visual alert threshold; distinct from the large ranking-bonus threshold (20).
constexpr double orange_burst_threshold = 5.0;
constexpr std::array<unsigned,6> visible_bits{1U,2U,4U,8U,16U,16U};
std::array<std::optional<double>,6> metric_values(const Process& row) {
    return {row.cpu_percent,
        row.working_set ? std::optional<double>(static_cast<double>(*row.working_set)) : std::nullopt,
        row.gpu_percent,
        row.dedicated_gpu_bytes ? std::optional<double>(static_cast<double>(*row.dedicated_gpu_bytes)) : std::nullopt,
        row.rx_bytes_per_second, row.tx_bytes_per_second};
}
double positive(std::optional<double> value) {
    return value && std::isfinite(*value) && *value > 0.0 ? *value : 0.0;
}
double fraction_score(double amount, std::optional<std::uint64_t> capacity, double weight) {
    if (!capacity || !*capacity) return 0.0; // Unknown is not treated as a small machine.
    return std::clamp(100.0 * amount / static_cast<double>(*capacity) * weight, 0.0, 100.0);
}
double activity_score(std::size_t metric, double value,
    std::optional<std::uint64_t> ram_capacity, std::optional<std::uint64_t> gpu_capacity) {
    switch (metric) {
    case 0: return std::clamp(value * 3.0, 0.0, 100.0); // 33.3% of all cores is high
    case 1: return fraction_score(value,ram_capacity,0.70);
    case 2: return std::clamp(value,0.0,100.0);       // GPU engine utilization
    case 3: return fraction_score(value,gpu_capacity,0.65);
    default: return std::clamp(90.0 * (1.0 - std::exp(-value / (2.0*mib))),0.0,90.0);
    }
}
double change_score(std::size_t metric, double increase,
    std::optional<std::uint64_t> ram_capacity, std::optional<std::uint64_t> gpu_capacity) {
    if (increase <= 0.0) return 0.0;
    switch (metric) {
    case 0: return std::min(100.0,increase * 4.0);
    case 1: return std::min(100.0,25.0*increase /
        std::max(gib,ram_capacity ? static_cast<double>(*ram_capacity) * .02 : 2.0*gib));
    case 2: return std::min(100.0,increase * 2.0);
    case 3: return std::min(100.0,30.0*increase /
        std::max(256.0*mib,gpu_capacity ? static_cast<double>(*gpu_capacity)*.08 : gib));
    default: return std::min(100.0,100.0*(1.0-std::exp(-increase/(2.0*mib))));
    }
}
} // namespace

void SpikeTracker::update(std::vector<Process>& rows,
    std::optional<std::uint64_t> ram_capacity, std::optional<std::uint64_t> gpu_capacity) {
    std::unordered_map<std::uint32_t, Baseline> next;
    next.reserve(rows.size());
    for (auto& row : rows) {
        row.spike_score = 0.0;
        row.auto_scores.fill(0.0);
        row.burst_scores.fill(0.0);
        const auto found = previous_.find(row.pid);
        const bool same = row.creation_time && found != previous_.end()
            && found->second.creation_time == row.creation_time;
        const auto measurements = metric_values(row);
        const auto old = same ? found->second.metrics
            : std::array<std::optional<double>,6>{};
        for (std::size_t i=0; i<measurements.size(); ++i) {
            if (!measurements[i] || !std::isfinite(*measurements[i]) || *measurements[i] < 0) continue;
            const double now = positive(measurements[i]);
            const double pressure = activity_score(i,now,ram_capacity,gpu_capacity);
            // EWMA prevents constant reordering while retaining steady high load.
            row.auto_scores[i] = same && old[i]
                ? 0.65*pressure + 0.35*found->second.auto_scores[i] : pressure;
            const double sudden = same && old[i]
                ? change_score(i,now-positive(old[i]),ram_capacity,gpu_capacity) : 0.0;
            row.spike_score = std::max(row.spike_score,sudden);
            // Brief orange hold at 1-second sampling; a burst is not permanent.
            row.burst_scores[i] = std::max(sudden,
                same ? found->second.burst_scores[i]*0.35 : 0.0);
        }
        next.emplace(row.pid, Baseline{row.creation_time, measurements,
            row.auto_scores, row.burst_scores});
    }
    previous_ = std::move(next);
}

AutoRank auto_rank(const Process& row, unsigned visible_metrics) {
    AutoRank result{};
    double top = 0.0, second = 0.0;
    double best_burst = 0.0;
    AutoMetric top_metric = AutoMetric::none, burst_metric = AutoMetric::none;
    int leading_group = -1;
    // Rx and Tx form one network category; do not double-count network.
    for (std::size_t i=0;i<row.auto_scores.size();++i) {
        if (!(visible_metrics & visible_bits[i])) continue;
        const double current = row.auto_scores[i];
        const int group = i >= 4 ? 4 : static_cast<int>(i);
        if (std::isfinite(current) && current > top) {
            if (group != leading_group) second = top;
            top = current;
            top_metric = static_cast<AutoMetric>(i+1);
            leading_group = group;
        } else if (group != leading_group && std::isfinite(current) && current > second) {
            second = current;
        }
        const double event = row.burst_scores[i];
        if (std::isfinite(event) && event > best_burst) {
            best_burst = event;
            burst_metric = static_cast<AutoMetric>(i+1);
        }
    }
    // Scores are in 0..100 points. Complementary demand is bounded to 25%.
    result.score = std::clamp(top + 0.25*second,0.0,100.0);
    // Positive changes are highlighted even below the separate 20-point ranking bonus.
    // Moving up because another process slowed down is not a burst.
    if (best_burst >= orange_burst_threshold) {
        if (best_burst >= 20.0) {
            result.score = std::clamp(std::max(result.score,0.85*best_burst)
                + 0.20*best_burst,0.0,100.0);
        }
        result.metric = burst_metric;
        result.burst = true;
    } else if (result.score >= 10.0) {
        result.metric = top_metric;
    }
    return result;
}
bool auto_metric_has_burst(const Process& row, AutoMetric metric) {
    const auto ordinal = static_cast<std::size_t>(metric);
    if (ordinal == 0 || ordinal > row.burst_scores.size()) return false;
    const auto burst = row.burst_scores[ordinal-1];
    return std::isfinite(burst) && burst >= orange_burst_threshold;
}

std::optional<double> auto_metric_load_percent(const Process& row, AutoMetric metric,
    std::optional<std::uint64_t> ram_capacity, std::optional<std::uint64_t> gpu_capacity) {
    const auto share = [](std::uint64_t used, std::optional<std::uint64_t> capacity) -> std::optional<double> {
        if (!capacity || *capacity == 0) return std::nullopt;
        return std::clamp(100.0 * static_cast<double>(used) / static_cast<double>(*capacity), 0.0, 100.0);
    };
    switch (metric) {
    case AutoMetric::cpu:
        if (row.cpu_percent && std::isfinite(*row.cpu_percent) && *row.cpu_percent >= 0.0)
            return std::clamp(*row.cpu_percent, 0.0, 100.0);
        break;
    case AutoMetric::ram:
        if (row.working_set) return share(*row.working_set, ram_capacity);
        break;
    case AutoMetric::gpu:
        if (row.gpu_percent && std::isfinite(*row.gpu_percent) && *row.gpu_percent >= 0.0)
            return std::clamp(*row.gpu_percent, 0.0, 100.0);
        break;
    case AutoMetric::vram:
        if (row.dedicated_gpu_bytes) return share(*row.dedicated_gpu_bytes, gpu_capacity);
        break;
    default: break; // Rx/Tx have no capacity denominator; no steady-state percentage color.
    }
    return std::nullopt;
}

namespace {
// Strict identity: no joining independent applications by name alone.
bool same_executable_family(const Process& child, const Process& parent) {
    if (!child.creation_time || !parent.creation_time ||
        *parent.creation_time >= *child.creation_time) return false;
    if (child.exe_path && parent.exe_path)
        return *child.exe_path == *parent.exe_path;
    // Both paths can be unavailable for protected processes. A verified parent
    // link, matching names and creation order are then the conservative fallback.
    return !child.exe_path && !parent.exe_path && child.name == parent.name;
}
void add_bytes(std::optional<std::uint64_t>& target, std::optional<std::uint64_t> value) {
    if (!value) return;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    target = *value > max - target.value_or(0) ? max : target.value_or(0) + *value;
}
void add_positive(std::optional<double>& target, std::optional<double> value) {
    if (!value || !std::isfinite(*value) || *value < 0) return;
    const double sum = target.value_or(0) + *value;
    if (std::isfinite(sum)) target = sum;
}
void max_gpu(std::optional<double>& target, std::optional<double> value) {
    if (value && std::isfinite(*value) && *value >= 0)
        target = std::max(target.value_or(0.0),*value);
}
} // namespace

std::vector<Process> aggregate_application_processes(const std::vector<Process>& rows) {
    std::unordered_map<std::uint32_t,std::size_t> positions;
    positions.reserve(rows.size());
    for (std::size_t i=0;i<rows.size();++i) positions.emplace(rows[i].pid,i);
    // Memoize roots. Parent links are strictly older, therefore acyclic;
    // each chain is resolved once rather than retraversed for every child.
    const auto unresolved=rows.size();
    std::vector<std::size_t> roots(rows.size(),unresolved);
    for (std::size_t i=0;i<rows.size();++i) {
        auto current=i;
        while (roots[current]==unresolved) {
            const auto found=positions.find(rows[current].parent_pid);
            if (found==positions.end() || found->second==current ||
                !same_executable_family(rows[current],rows[found->second])) {
                roots[current]=current;
                break;
            }
            current=found->second;
        }
        const auto root=roots[current];
        current=i;
        while (roots[current]==unresolved) {
            const auto parent=positions.find(rows[current].parent_pid)->second;
            roots[current]=root;
            current=parent;
        }
    }
    // Same canonical full EXE path identifies an installed application, even
    // when its workers have disconnected parent chains. A PID/name alone is
    // never enough to merge unrelated processes. Paths were case-normalized
    // by ProcessCollector; private/unknown paths use verified parent lineage.
    // Keys borrow the stable input paths for this call; no per-group path copies.
    std::unordered_map<std::string_view,std::size_t> image_groups;
    std::unordered_map<std::size_t,std::size_t> lineage_groups;
    image_groups.reserve(rows.size());
    lineage_groups.reserve(rows.size());
    std::vector<Process> result;
    result.reserve(rows.size());
    for (std::size_t i=0;i<rows.size();++i) {
        const Process& root=rows[roots[i]];
        const auto& image=rows[i].exe_path ? rows[i].exe_path : root.exe_path;
        const auto next=result.size();
        std::size_t group_index=next;
        if (image && !image->empty()) {
            group_index=image_groups.try_emplace(*image,next).first->second;
        } else {
            group_index=lineage_groups.try_emplace(roots[i],next).first->second;
        }
        if (group_index==next) {
            Process combined=root;
            combined.cpu_percent.reset(); combined.working_set.reset(); combined.private_bytes.reset();
            combined.gpu_percent.reset(); combined.dedicated_gpu_bytes.reset();
            combined.presented_fps.reset();
            combined.rx_bytes_per_second.reset(); combined.tx_bytes_per_second.reset();
            combined.spike_score=0; combined.auto_scores.fill(0); combined.burst_scores.fill(0);
            combined.member_pids.clear();
            result.push_back(std::move(combined));
        }
        auto& combined=result[group_index];
        const auto& part=rows[i];
        combined.member_pids.push_back(part.pid);
        add_positive(combined.cpu_percent,part.cpu_percent);
        add_bytes(combined.working_set,part.working_set);
        add_bytes(combined.private_bytes,part.private_bytes);
        max_gpu(combined.gpu_percent,part.gpu_percent); // Never sum independent WDDM engines.
        add_bytes(combined.dedicated_gpu_bytes,part.dedicated_gpu_bytes);
        add_positive(combined.rx_bytes_per_second,part.rx_bytes_per_second);
        add_positive(combined.tx_bytes_per_second,part.tx_bytes_per_second);
    }
    for (auto& group : result)
        if (group.cpu_percent) group.cpu_percent=std::clamp(*group.cpu_percent,0.0,100.0);
    return result;
}

namespace {
// Shared ordering ensures pointer sorting and owned-row sorting remain identical.
bool process_before(const Process& a,const Process& b,Sort sort,unsigned visible_metrics) {
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
    const auto av=value(a);
    const auto bv=value(b);
    if (av.has_value()!=bv.has_value()) return av.has_value();
    if (av && *av!=*bv) return *av>*bv;
    if (sort==Sort::automatic && a.cpu_percent!=b.cpu_percent) {
        if (a.cpu_percent.has_value()!=b.cpu_percent.has_value()) return a.cpu_percent.has_value();
        return *a.cpu_percent>*b.cpu_percent;
    }
    return a.pid<b.pid;
}
} // namespace

void sort_processes(std::vector<Process>& rows,Sort sort,unsigned visible_metrics) {
    std::sort(rows.begin(),rows.end(),[sort,visible_metrics](const Process& a,const Process& b) {
        return process_before(a,b,sort,visible_metrics);
    });
}
void sort_process_refs(std::vector<const Process*>& rows,Sort sort,unsigned visible_metrics) {
    std::sort(rows.begin(),rows.end(),[sort,visible_metrics](const Process* a,const Process* b) {
        return process_before(*a,*b,sort,visible_metrics);
    });
}

// Called after auto-ranking, not as part of resource scoring.
// Stable rotation keeps every other process in its original ranking order.
bool prioritize_focused_process(std::vector<Process>& rows, std::uint32_t pid) {
    if (!pid) return false;
    const auto position = std::find_if(rows.begin(),rows.end(),[pid](const Process& p) {
        return p.pid == pid || std::find(p.member_pids.begin(),p.member_pids.end(),pid)
            != p.member_pids.end();
    });
    if (position == rows.end()) return false;
    if (position != rows.begin()) std::rotate(rows.begin(),position,position+1);
    return true;
}
// Pointer overload avoids copying Process objects when the foreground changes.
bool prioritize_focused_process(std::vector<const Process*>& rows, std::uint32_t pid) {
    if (!pid) return false;
    const auto position=std::find_if(rows.begin(),rows.end(),[pid](const Process* p) {
        return p->pid==pid || std::find(p->member_pids.begin(),p->member_pids.end(),pid)!=p->member_pids.end();
    });
    if (position==rows.end()) return false;
    if (position!=rows.begin()) std::rotate(rows.begin(),position,position+1);
    return true;
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
