#pragma once

#include "fluxtop/system_graph_history.hpp"
#include <algorithm>
#include <array>

namespace fluxtop {
enum class OverlayChartKind { cpu, cpu_temperature, ram, gpu, vram, network, fps, disk, core };
struct OverlayChart {
    OverlayChartKind kind;
    std::wstring title, key;
    wchar_t disk{};
    CpuViewId core{};
    bool enabled{};
};
struct OverlayChartValues {
    std::optional<double> primary, secondary;
};
inline OverlayChartValues overlay_chart_values(const OverlayChart& chart,const SystemChartPoint& point) {
    switch(chart.kind) {
    case OverlayChartKind::cpu:return {point.values[0],{}};
    case OverlayChartKind::cpu_temperature:return {point.cpu.package,{}};
    case OverlayChartKind::ram:return {point.values[1],{}};
    case OverlayChartKind::gpu:return {point.values[3],{}};
    case OverlayChartKind::vram:return {point.values[4],{}};
    case OverlayChartKind::network:return {point.values[2],{}};
    case OverlayChartKind::fps:return {point.fps?std::optional{point.fps->value}:std::nullopt,{}};
    case OverlayChartKind::disk:{
        const auto index=static_cast<unsigned>(chart.disk-L'A');
        if(index<point.disks.size())return {point.disks[index],point.disk_temperatures[index]};
        return {};
    }
    case OverlayChartKind::core:return {
        cpu_chart_value({CpuSeriesKind::core_load,chart.core,{},false},point.cpu,point.values[0]),
        cpu_chart_value({CpuSeriesKind::core,chart.core,{},false},point.cpu,{})};
    }
    return {};
}
struct OverlayChartLayout {
    std::size_t columns{}, capacity{};
    float height{}, tile_height{}, gap{};
};
inline OverlayChartLayout overlay_chart_layout(float width,float available_height,float font,
    std::size_t selected) noexcept {
    if(!selected)return {};
    const float gap=std::max(4.0f,font*.35f);
    const float tile_height=std::max(45.0f,font*3.3f);
    const auto columns=static_cast<std::size_t>(std::clamp(
        width/std::max(132.0f,font*11.0f),1.0f,3.0f));
    const float budget=std::min(available_height*.5f,available_height-font*1.6f-4);
    if(budget<tile_height+gap)return {};
    // Leave room for a page indicator, including on the last page.
    const auto fit=static_cast<std::size_t>(std::max(1.0f,(budget-gap-font)/(tile_height+gap)));
    const auto rows=std::min(fit,(selected+columns-1)/columns);
    const auto capacity=rows*columns;
    const auto height=static_cast<float>(rows)*(tile_height+gap)+(selected>capacity?font+gap:gap);
    if(height>budget)return {};
    return {columns,capacity,height,tile_height,gap};
}
} // namespace fluxtop
