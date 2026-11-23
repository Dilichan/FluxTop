#pragma once
// Pure, UI-independent chart controls. All values are view state only:
// they must never change sampler cadence or SQLite history writes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fluxtop {
struct ChartTimeWindow { std::int64_t from{}, to{}; };
struct ChartPairSplit { long first_bottom{},second_top{}; };
constexpr ChartPairSplit chart_pair_split(long top,long bottom) noexcept {
    const long middle=top+(bottom-top)/2;
    const long gap=std::min(100L,std::max(0L,bottom-top)/3);
    return {middle-gap/2,middle+(gap-gap/2)};
}

inline std::int64_t chart_system_duration(unsigned range) noexcept {
    constexpr std::array<std::int64_t,4> durations{60000,300000,900000,1800000};
    return durations[std::min<unsigned>(range,3U)];
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
inline ChartTimeWindow chart_popup_window(ChartTimeWindow base,double zoom_x,
    double pan_ms) noexcept {
    const double total=static_cast<double>(std::max<std::int64_t>(1000,base.to-base.from));
    const double x=std::isfinite(zoom_x)?std::clamp(zoom_x,1.0,128.0):1.0;
    const double span=std::max(1000.0,total/x);
    const double room=std::max(0.0,(total-span)/2.0);
    const double pan=std::isfinite(pan_ms)?std::clamp(pan_ms,-room,room):0.0;
    const double center=(static_cast<double>(base.from)+static_cast<double>(base.to))/2.0+pan;
    auto from=static_cast<std::int64_t>(center-span/2.0);
    auto to=static_cast<std::int64_t>(center+span/2.0);
    if(to<=from)to=from+1000;
    return {from,to};
}
} // namespace fluxtop
