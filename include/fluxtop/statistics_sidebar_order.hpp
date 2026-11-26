#pragma once
// Maps visual sidebar rows to stable StatisticsPage section IDs.
// Drives are inserted between GPU and History. Stored section IDs stay stable.
#include <optional>

namespace fluxtop {
enum class StatisticsSidebarKind { section, disk, core };
inline constexpr unsigned fps_section=7;
struct StatisticsSidebarItem {
    StatisticsSidebarKind kind{};
    unsigned index{}; // Section ID or index into the corresponding device list.
};

constexpr unsigned statistics_sidebar_section_row(unsigned section,unsigned disk_count,unsigned core_count=0) noexcept {
    if(section==4U)return 0U;            // AutoScore first.
    if(section==0U)return 1U;
    if(section==fps_section)return 2U;
    if(section<4U)return section+2U+core_count;
    if(section==5U || section==6U)return section+1U+disk_count+core_count;
    return ~0U;
}
constexpr unsigned statistics_sidebar_core_row(unsigned core_index) noexcept {
    return core_index+3U;
}
constexpr unsigned statistics_sidebar_disk_row(unsigned disk_index,unsigned core_count=0) noexcept {
    return disk_index+6U+core_count;
}
constexpr std::optional<StatisticsSidebarItem> statistics_sidebar_item_at(
    unsigned row,unsigned disk_count,unsigned core_count=0) noexcept {
    if(row==0U)return StatisticsSidebarItem{StatisticsSidebarKind::section,4U};
    if(row==1U)return StatisticsSidebarItem{StatisticsSidebarKind::section,0U};
    if(row==2U)return StatisticsSidebarItem{StatisticsSidebarKind::section,fps_section};
    if(row-3U<core_count)return StatisticsSidebarItem{StatisticsSidebarKind::core,row-3U};
    row-=core_count;
    if(row<6U)return StatisticsSidebarItem{StatisticsSidebarKind::section,row-2U};
    if(row-6U<disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::disk,row-6U};
    if(row==6U+disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::section,5U};
    if(row==7U+disk_count)
        return StatisticsSidebarItem{StatisticsSidebarKind::section,6U};
    return std::nullopt;
}
} // namespace fluxtop
