#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace fluxtop {
// A score is the existing AutoRank heuristic, evaluated with all six resource inputs.
// Historic scores begin when schema 2 starts recording them; older raw metrics remain intact.
struct StatisticsApplication {
    std::int64_t id{};
    std::wstring name;
    std::wstring path;
    std::wstring identity; // Exact persisted identity, including lifetime fallback.
    double observed_seconds{};
    std::int64_t last_seen_ms{};
    std::vector<std::pair<std::int64_t,double>> points;
};
struct StatisticsHistory {
    std::int64_t first_record_ms{};
    std::int64_t last_record_ms{};
    bool scores_available{};
    std::vector<StatisticsApplication> uptime;
    std::vector<StatisticsApplication> history;
    std::vector<StatisticsApplication> recent;
    std::wstring error;
};
// UTC epoch milliseconds; start is inclusive, end is exclusive. All DB reads
// are read-only and cancellable; the UI must call this on a worker thread.
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned limit,bool include_system,
    std::stop_token stop);
// Independent result caps for historical graph, uptime list, and live chart.
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned history_limit,
    unsigned uptime_limit,unsigned live_limit,bool include_system,std::stop_token stop);
// Uptime uses an independent time window; history/recent keep their own windows.
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned history_limit,
    unsigned uptime_limit,unsigned live_limit,
    std::int64_t uptime_from_ms,std::int64_t uptime_to_ms,
    bool include_system,std::stop_token stop,bool uptime_only=false);
// Use the real Windows directory, not a hard-coded system-drive assumption.
bool statistics_is_system(std::wstring_view name,std::wstring_view path);
}