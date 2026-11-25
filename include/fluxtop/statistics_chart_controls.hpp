#pragma once
// Pure, UI-independent chart controls. All values are view state only:
// they must never change sampler cadence or SQLite history writes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fluxtop {
struct ChartTimeWindow { std::int64_t from{}, to{}; };
struct ChartPairSplit { long first_bottom{},second_top{}; };
constexpr ChartPairSplit chart_pair_split(long top,long bottom) noexcept {
    const long middle=top+(bottom-top)/2;
    const long gap=std::min(100L,std::max(0L,bottom-top)/3);
    return {middle-gap/2,middle+(gap-gap/2)};
}

// System kinds (2 and above) share the detached windows' stored autoscale keys.
struct SystemChartPanel { unsigned metric; std::wstring_view caption{}; };
struct SystemChartPanels { SystemChartPanel primary; std::optional<SystemChartPanel> secondary; };
constexpr SystemChartPanels chart_system_panels(unsigned kind) noexcept {
    if(kind==2)return {{0,L"Загрузка CPU"},SystemChartPanel{31,L"Температура CPU"}};
    if(kind==5)return {{3,L"Загрузка GPU"},SystemChartPanel{4,L"Использование VRAM"}};
    if(kind>=32)return {{32U+2U*(kind-32U),L"Загрузка "},SystemChartPanel{33U+2U*(kind-32U),L"Температура "}};
    return {{kind>=6?kind-1:kind-2},{}}; // Disks, RAM, network
}

struct ChartBox { long left,top,right,bottom; };
struct DetachedChartLayout {
    std::array<ChartBox,3> controls; // Autoscale, fill, pause
    long title_top;
    ChartBox plot;
};
constexpr DetachedChartLayout chart_detached_layout(long width,long height) noexcept {
    constexpr long margin=22, gap=12, button_height=28;
    constexpr std::array<long,3> widths{150,160,126};
    const long available=std::max(1L,width-2*margin);
    DetachedChartLayout layout{};
    long x=margin,y=8;
    for(std::size_t index=0;index<widths.size();++index) {
        const auto control_width=std::min(widths[index],available);
        if(x!=margin && x+control_width>margin+available) {
            x=margin;
            y+=button_height+8;
        }
        layout.controls[index]={x,y,x+control_width,y+button_height};
        x+=control_width+gap;
    }
    layout.title_top=y+button_height+15;
    layout.plot={116,layout.title_top+59,width-30,height-60};
    return layout;
}

struct SystemChartPeriod { const wchar_t* label; std::int64_t duration_ms; };
inline constexpr std::array<SystemChartPeriod,4> system_chart_periods{{
    {L"1 минута",60000},{L"5 минут",300000},{L"15 минут",900000},{L"30 минут",1800000}}};
constexpr unsigned chart_system_period_index(unsigned range) noexcept {
    return std::min(range,static_cast<unsigned>(system_chart_periods.size()-1));
}
constexpr std::int64_t chart_system_duration(unsigned range) noexcept {
    return system_chart_periods[chart_system_period_index(range)].duration_ms;
}
inline ChartTimeWindow chart_window(bool paused,ChartTimeWindow frozen,
    ChartTimeWindow live) noexcept {
    return paused ? frozen : live;
}
inline double chart_effective_zoom(bool autoscale,double automatic,double manual) noexcept {
    const double value=autoscale?automatic:manual;
    return std::isfinite(value) ? std::clamp(value,0.01,100.0) : 1.0;
}
// Pan and horizontal zoom are owned by the individual detached graph.
// A paused graph uses the frozen range as its source; live graphs use the
// current model period. Neither branch changes the underlying observations.
inline double chart_popup_pan_limit(ChartTimeWindow base,double zoom_x) noexcept {
    const double total=static_cast<double>(std::max<std::int64_t>(1000,base.to-base.from));
    const double x=std::isfinite(zoom_x)?std::clamp(zoom_x,1.0,128.0):1.0;
    return (total-std::max(1000.0,total/x))/2.0;
}
inline ChartTimeWindow chart_popup_window(ChartTimeWindow base,double zoom_x,double pan_ms) noexcept {
    const double room=chart_popup_pan_limit(base,zoom_x);
    const double pan=std::isfinite(pan_ms)?std::clamp(pan_ms,-room,room):0.0;
    const double center=(static_cast<double>(base.from)+static_cast<double>(base.to))/2.0+pan;
    const double half_span=static_cast<double>(std::max<std::int64_t>(1000,base.to-base.from))/2.0-room;
    auto from=static_cast<std::int64_t>(center-half_span);
    auto to=static_cast<std::int64_t>(center+half_span);
    if(to<=from)to=from+1000;
    return {from,to};
}
} // namespace fluxtop
