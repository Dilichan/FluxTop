#pragma once
// AutoScore Y-axis fitted to data actually visible at the current time zoom.
// A long segment may pass through the viewport even when neither endpoint
// is inside it. Interpolate those boundaries instead of falling back to 0..100.
#include "fluxtop/statistics_history.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fluxtop {
inline double statistics_auto_vertical_zoom(
    const std::vector<StatisticsApplication>& apps,
    std::int64_t from, std::int64_t to) noexcept {
    if(to<=from) return 1.0;
    double peak=0.0;
    bool found=false;
    const auto consider=[&](double score){
        if(!std::isfinite(score)) return;
        found=true;
        peak=std::max(peak,std::clamp(score,0.0,100.0));
    };
    for(const auto& app:apps) {
        const auto& points=app.points;
        for(std::size_t i=0;i<points.size();++i) {
            const auto& point=points[i];
            if(point.first>=from && point.first<=to) consider(point.second);
            if(i==0) continue;
            const auto& previous=points[i-1];
            if(point.first<=previous.first || previous.first>to || point.first<from ||
               !std::isfinite(previous.second) || !std::isfinite(point.second)) continue;
            const auto left=std::max(from,previous.first);
            const auto right=std::min(to,point.first);
            if(left>right) continue;
            const auto interpolate=[&](std::int64_t at){
                const auto fraction=static_cast<double>(at-previous.first)/
                    static_cast<double>(point.first-previous.first);
                return previous.second+(point.second-previous.second)*fraction;
            };
            consider(interpolate(left));
            consider(interpolate(right));
        }
    }
    if(!found) return 1.0;
    const double padded=std::clamp(peak*1.18,2.0,100.0);
    const double step=padded<=10.0?1.0:padded<=25.0?2.0:padded<=50.0?5.0:10.0;
    const double ceiling=std::clamp(std::ceil(padded/step)*step,2.0,100.0);
    return 100.0/ceiling;
}
// Present the most recent live samples just outside the right clip edge.
// The plot remains left-anchored; history (overscan=0) keeps exact coordinates.
inline double statistics_chart_project_x(double x,double left,double right,double overscan) noexcept {
    if(right<=left || overscan<=0.0)return x;
    return left+(x-left)*(right-left+overscan)/(right-left);
}
inline double statistics_chart_unproject_x(double x,double left,double right,double overscan) noexcept {
    if(right<=left || overscan<=0.0)return x;
    return left+(x-left)*(right-left)/(right-left+overscan);
}
} // namespace fluxtop
