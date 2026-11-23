#pragma once
// Maps visual sidebar rows to stable StatisticsPage section IDs.
// Drives are inserted between GPU and History, without changing stored section IDs.
#include <optional>

namespace fluxtop {
enum class StatisticsSidebarKind { section, disk, core };
struct StatisticsSidebarItem {
    StatisticsSidebarKind kind{};
    unsigned index{}; // Section ID (0..6) or index into the detected disk-letter list.
};

constexpr unsigned statistics_sidebar_section_row(unsigned section,unsigned disk_count,unsigned core_count=0) noexcept {
    if(section==4U)return 0U;            // AutoScore first.
    if(section==0U)return 1U;
    if(section<4U)return section+1U+core_count;
    if(section==5U || section==6U)return section+disk_count+core_count;
    return ~0U;
}
constexpr unsigned statistics_sidebar_disk_row(unsigned disk_index,unsigned core_count=0) noexcept {
    return disk_index+5U+core_count;
}
constexpr std::optional<StatisticsSidebarItem> statistics_sidebar_item_at(
    unsigned row,unsigned disk_count,unsigned core_count=0) noexcept {
    if(row==0U)return StatisticsSidebarItem{StatisticsSidebarKind::section,4U};
    if(row==1U)return StatisticsSidebarItem{StatisticsSidebarKind::section,0U};
    if(row-2U<core_count)return StatisticsSidebarItem{StatisticsSidebarKind::core,row-2U};
    row-=core_count;
    if(row<5U)return StatisticsSidebarItem{StatisticsSidebarKind::section,row-1U};
    if(row-5U<disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::disk,row-5U};
    if(row==5U+disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::section,5U};
    if(row==6U+disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::section,6U};
    return std::nullopt;
}
} // namespace fluxtop
