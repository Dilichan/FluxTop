#pragma once
// AutoScore Y-axis fitted to data actually visible at the current time zoom.
// A long segment may pass through the viewport even when neither endpoint
// is inside it. Interpolate those boundaries instead of falling back to 0..100.
#include "fluxtop/statistics_history.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace fluxtop {
inline double statistics_auto_vertical_zoom(
    const std::vector<StatisticsApplication>& apps,
    std::int64_t from, std::int64_t to) noexcept {
    if(to<=from) return 1.0;
    // Sample the visible portions of segments uniformly in *time*, not by
    // number of database rows. One isolated high point must not flatten days
    // of ordinary activity; sustained high demand must remain high on the axis.
    // Scores are bounded to 0..100 and never altered in the underlying series.
    constexpr std::size_t bins=101;
    constexpr int samples_per_segment=9;
    std::array<double,bins> time_weights{};
    double maximum=0.0;
    bool found=false;
    const auto consider=[&](double score,double duration){
        if(!std::isfinite(score) || !std::isfinite(duration) || duration<=0.0)return;
        const double bounded=std::clamp(score,0.0,100.0);
        maximum=std::max(maximum,bounded);
        found=true;
        const auto bucket=static_cast<std::size_t>(std::clamp(
            static_cast<int>(std::ceil(bounded)),0,100));
        time_weights[bucket]+=duration;
    };
    for(const auto& app:apps) {
        const auto& points=app.points;
        // A lone visible point must still produce a meaningful scale.
        if(points.size()==1 && points[0].first>=from && points[0].first<=to)
            consider(points[0].second,1.0);
        for(std::size_t i=1;i<points.size();++i) {
            const auto& a=points[i-1];
            const auto& b=points[i];
            if(b.first<=a.first || a.first>to || b.first<from ||
                !std::isfinite(a.second) || !std::isfinite(b.second))continue;
            const auto left=std::max(from,a.first);
            const auto right=std::min(to,b.first);
            if(left>=right) {
                if(left==right && left>=from && left<=to)
                    consider(a.second+(b.second-a.second)*
                        static_cast<double>(left-a.first)/static_cast<double>(b.first-a.first),1.0);
                continue;
            }
            const double duration=static_cast<double>(right-left);
            for(int sample=0;sample<samples_per_segment;++sample) {
                const double at=static_cast<double>(left)+duration*
                    (static_cast<double>(sample)+0.5)/samples_per_segment;
                const double fraction=(at-static_cast<double>(a.first))/
                    static_cast<double>(b.first-a.first);
                consider(a.second+(b.second-a.second)*fraction,
                    duration/samples_per_segment);
            }
        }
    }
    if(!found)return 1.0;
    const double total=std::accumulate(time_weights.begin(),time_weights.end(),0.0);
    // Fewer than approximately twenty independent time slices have too little
    // evidence to distinguish a spike from sustained load. In that case retain
    // the exact peak (including long, sparsely sampled segments).
    const double window=static_cast<double>(to-from);
    const bool enough_time=window>0.0 && total>=window*0.95;
    double peak=maximum;
    if(enough_time) {
        const double threshold=total*0.95;
        double cumulative=0.0;
        for(std::size_t bin=0;bin<bins;++bin) {
            cumulative+=time_weights[bin];
            if(cumulative>=threshold) {peak=static_cast<double>(bin);break;}
        }
    }
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
