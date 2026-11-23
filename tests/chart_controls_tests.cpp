#include "fluxtop/statistics_chart_controls.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace fluxtop;
static void check(bool pass,const char* message) {
    if(!pass)throw std::runtime_error(message);
}
int main() {
    try {
        for(const long height:{640L,820L,1080L}){
            const auto split=chart_pair_split(114,height-100);
            check(split.first_bottom>114 && split.second_top<height-100,
                "Paired charts retain positive plot heights at supported window sizes");
            check(split.first_bottom+24+18<split.second_top-28,
                "Upper chart dates must clear the lower chart caption after resizing");
        }
        check(chart_system_duration(0)==60000 && chart_system_duration(1)==300000 &&
            chart_system_duration(2)==900000 && chart_system_duration(3)==1800000 && chart_system_duration(99)==1800000,
            "System graph presets must share their 1/5/15/30-minute timing");
        const ChartTimeWindow frozen{100,200},live{300,400};
        const auto a=chart_window(true,frozen,live),b=chart_window(false,frozen,live);
        check(a.from==100 && a.to==200 && b.from==300 && b.to==400,
            "Paused views freeze only the selected chart's time interval");
        check(chart_effective_zoom(true,6.0,2.0)==6.0 &&
            chart_effective_zoom(false,6.0,2.0)==2.0 &&
            chart_effective_zoom(false,6.0,0.0)==0.01,
            "Auto and manual scale must not overwrite each other");
        const ChartTimeWindow source{1000,61000};
        const auto full=chart_popup_window(source,1.0,0.0);
        check(full.from==1000 && full.to==61000,
            "Default popup must display the complete source interval");
        const auto zoomed=chart_popup_window(source,2.0,0.0);
        check(zoomed.from==16000 && zoomed.to==46000,
            "Popup zoom must use the selected interval center");
        const auto right=chart_popup_window(source,2.0,999999.0);
        check(right.to==61000 && right.from==31000,
            "Popup pan must remain within available history");
        const auto left=chart_popup_window(source,2.0,-999999.0);
        check(left.from==1000 && left.to==31000,
            "Opposite popup pan must remain within history");
        const auto independent=chart_popup_window(source,1.0,0.0);
        check(independent.from==full.from && independent.to==full.to,
            "Zooming a detached chart must not mutate another chart's period");
        const auto resumed=chart_window(false,{1000,61000},{62000,122000});
        check(resumed.from==62000 && resumed.to==122000 &&
            chart_window(true,{1000,61000},{62000,122000}).to==61000,
            "Resuming one view must not advance other paused views");
        const auto short_range=chart_popup_window({0,500},128.0,0.0);
        check(short_range.to>short_range.from,
            "Very short windows must preserve a nonzero duration");
        const auto nan=chart_popup_window(source,2.0,NAN);
        check(nan.from==zoomed.from && nan.to==zoomed.to,
            "Invalid pan input must not corrupt chart coordinates");
        std::cout << "Independent chart zoom/pause/range and paired-label spacing contracts passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
