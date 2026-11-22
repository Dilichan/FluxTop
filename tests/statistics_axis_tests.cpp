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
        // v41: distribution, not the single tallest point, determines Y scale.
        series[0].points.clear();
        for(std::int64_t t=0;t<=100000;t+=1000)
            series[0].points.push_back({t,t==50000?95.0:3.0});
        const auto spike_zoom=fluxtop::statistics_auto_vertical_zoom(series,0,100000);
        check(spike_zoom>5.0,
            "An isolated short peak must not compress a long low-load history");
        check(series[0].points[50].second==95.0,
            "Autoscaling cannot mutate or delete the true outlier");
        for(auto& point:series[0].points)point.second=85.0;
        check(fluxtop::statistics_auto_vertical_zoom(series,0,100000)==1.0,
            "Sustained high load must retain a sufficiently tall axis");
        series[0].points.clear();
        check(fluxtop::statistics_auto_vertical_zoom(series,0,100000)==1.0,
            "Empty series must retain the safe default axis");
        series[0].points={{50000,0.5}};
        check(fluxtop::statistics_auto_vertical_zoom(series,0,100000)==50.0,
            "Single low sample must remain visible");
        series[0].points.clear();
        for(std::int64_t t=0;t<=100000;t+=1000)
            series[0].points.push_back({t,(t>=40000 && t<=60000)?80.0:3.0});
        check(fluxtop::statistics_auto_vertical_zoom(series,0,100000)==1.0,
            "A sustained twenty-percent peak interval must not be hidden");
        check(fluxtop::statistics_auto_vertical_zoom(series,0,30000)>5.0,
            "Panning to a quiet interval must recalculate the Y scale");
        check(fluxtop::statistics_auto_vertical_zoom(series,40000,60000)==1.0,
            "Panning to a sustained busy interval must recalculate the Y scale");
        // Time windows analogous to 24h, 7 days and a custom interval.
        series[0].points={{0,2.0},{86400000,2.0},{2*86400000,90.0},
            {3*86400000,2.0},{7*86400000,2.0}};
        check(fluxtop::statistics_auto_vertical_zoom(series,0,86400000)>1.0,
            "24-hour viewport must use its own visible data");
        check(fluxtop::statistics_auto_vertical_zoom(series,0,7*86400000)>=1.0,
            "7-day viewport must return a finite valid zoom");
        check(fluxtop::statistics_auto_vertical_zoom(series,86400000,3*86400000)>=1.0,
            "Custom viewport must return a valid zoom");
        std::cout<<"Clipped historical AutoScore autoscale tests passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
