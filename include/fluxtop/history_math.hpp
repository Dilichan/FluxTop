#pragma once
// Constant-space, mergeable time-weighted statistics.
// Combining buckets adds weighted numerators and denominators, NOT averages.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace fluxtop {
inline constexpr std::int64_t history_minute_ms = 60'000;
inline constexpr std::int64_t history_hour_ms = 3'600'000;
inline constexpr std::int64_t history_day_ms = 86'400'000;
inline constexpr std::int64_t history_retention_days = 3'660; // At least ten calendar years.
inline constexpr std::uint64_t history_emergency_bytes = 50ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t history_stop_bytes = 49ULL * 1024 * 1024 * 1024;

enum class HistoryMetric : std::size_t { score, cpu, ram, gpu, vram, rx, tx, count };
inline constexpr std::size_t history_metric_count=static_cast<std::size_t>(HistoryMetric::count);

struct WeightedValue {
    double integral{};
    double correction{}; // Neumaier compensated sum, cheap insurance against drift.
    std::int64_t duration_ms{};
    std::uint64_t samples{};
    double minimum=std::numeric_limits<double>::infinity();
    double maximum=-std::numeric_limits<double>::infinity();
    void add(std::optional<double> value,std::int64_t duration) noexcept {
        if(!value || !std::isfinite(*value) || duration<=0) return;
        const double part=*value*static_cast<double>(duration);
        if(!std::isfinite(part)) return;
        const double next=integral+part;
        if(std::abs(integral)>=std::abs(part)) correction+=(integral-next)+part;
        else correction+=(part-next)+integral;
        integral=next;
        duration_ms+=duration;
        ++samples;
        minimum=std::min(minimum,*value);
        maximum=std::max(maximum,*value);
    }
    void merge(const WeightedValue& b) noexcept {
        if(!b.duration_ms) return;
        const double part=b.integral+b.correction;
        const double next=integral+part;
        if(std::abs(integral)>=std::abs(part)) correction+=(integral-next)+part;
        else correction+=(part-next)+integral;
        integral=next;
        duration_ms+=b.duration_ms;
        samples+=b.samples;
        minimum=std::min(minimum,b.minimum);
        maximum=std::max(maximum,b.maximum);
    }
    [[nodiscard]] std::optional<double> average() const noexcept {
        if(duration_ms<=0) return std::nullopt;
        return (integral+correction)/static_cast<double>(duration_ms);
    }
    [[nodiscard]] double numerator() const noexcept {return integral+correction;}
};
struct HistoryAccumulator {
    std::array<WeightedValue,history_metric_count> values{};
    std::int64_t uptime_ms{};
    std::int64_t last_seen_ms{};
    void observe(const std::array<std::optional<double>,history_metric_count>& readings,
                 std::int64_t duration, std::int64_t observed_at) noexcept {
        if(duration<=0) return;
        uptime_ms+=duration;
        last_seen_ms=std::max(last_seen_ms,observed_at);
        for(std::size_t i=0;i<values.size();++i) values[i].add(readings[i],duration);
    }
};
// Signed timestamps are currently positive (Unix epoch ms); mathematical floor
// also supports test timestamps before 1970.
inline constexpr std::int64_t history_floor(std::int64_t value,std::int64_t width) noexcept {
    const auto remainder=value%width;
    return value-(remainder<0?remainder+width:remainder);
}
}
