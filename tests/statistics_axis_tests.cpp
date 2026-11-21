#include "fluxtop/statistics_axis.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(){
    try {
        fluxtop::StatisticsApplication app;
        app.name=L"sample.exe";
        app.points={{0,1.0},{10000,1.0},{20000,1.0}};
        std::vector<fluxtop::StatisticsApplication> series{app};
        check(std::abs(fluxtop::statistics_auto_vertical_zoom(series,12000,18000)-50.0)<0.001,
            "Zoom into sparse flat segment must use Y max 2, not 100");
        // Both endpoints are outside viewport, but the graph draws that segment.
        series[0].points={{0,0.4},{60000,0.4}};
        check(std::abs(fluxtop::statistics_auto_vertical_zoom(series,20000,30000)-50.0)<0.001,
            "Viewport-crossing line must be included in autoscale");
        series[0].points={{0,90.0},{10000,0.5}};
        const auto near_tail=fluxtop::statistics_auto_vertical_zoom(series,9000,9500);
        check(near_tail>1.0 && near_tail<20.0,
            "Viewport scale must use interpolated boundary rather than distant 90-peak");
        check(fluxtop::statistics_auto_vertical_zoom(series,15000,16000)==1.0,
            "No visible line should retain safe 0..100 scale");
        series[0].points={{20000,3.0}};
        check(fluxtop::statistics_auto_vertical_zoom(series,19000,21000)>1.0,
            "Single in-view point must scale");
        check(fluxtop::statistics_auto_vertical_zoom(series,21000,19000)==1.0,
            "Invalid viewport must be safe");
        series[0].points={{0,0.4},{10000,0.4},{20000,90.0}};
        const double quiet_period=fluxtop::statistics_auto_vertical_zoom(series,0,10000);
        const double busy_period=fluxtop::statistics_auto_vertical_zoom(series,10000,20000);
        check(quiet_period>busy_period && quiet_period>1.0 && busy_period==1.0,
            "History autoscale must use the selected period, not stale earlier peaks");
        // The left edge never moves; the latest sample is painted offscreen.
        const double left=42.0,right=842.0,guard=18.0;
        const auto project=[&](double x){
            return fluxtop::statistics_chart_project_x(x,left,right,guard);
        };
        const auto unproject=[&](double x){
            return fluxtop::statistics_chart_unproject_x(x,left,right,guard);
        };
        check(project(left)==left,"Live overscan keeps the left chart edge unchanged");
        check(std::abs(project(right)-(right+guard))<0.001,
            "Live newest sample must enter outside the right clipping boundary");
        check(project(400.0)>400.0 && project(400.0)<right,
            "Mid-viewport history must still draw within the visible plot");
        check(std::abs(unproject(project(400.0))-400.0)<0.001,
            "Chart hover must invert the right-edge projection");
        check(fluxtop::statistics_chart_project_x(400.0,left,right,0.0)==400.0,
            "Historical chart geometry must remain unchanged");
        std::cout<<"Clipped historical AutoScore autoscale tests passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
