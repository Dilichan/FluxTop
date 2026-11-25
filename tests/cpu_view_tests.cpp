#include "fluxtop/cpu_view.hpp"
#include "fluxtop/overlay_ini.hpp"
#include "fluxtop/statistics_sidebar_order.hpp"
#include <filesystem>
#include <map>
#include <iostream>
#include <stdexcept>
using namespace fluxtop;
namespace {
void need(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
CpuTopologySnapshot topology(){
    CpuTopologySnapshot t;t.cpu_name=L"Synthetic physical CPU";
    t.cores={{0,0,0,true,61.0,2,false},{0,2,1,false,48.0,1,true},{1,0,2,true,std::nullopt,2,false}};
    t.logical_members={{0,0,0,true},{0,1,0,true},{0,2,1,false},{1,0,2,true},{1,1,2,true}};
    t.package_c=65;t.core_max_c=61;t.core_average_c=54.5;return t;
}
void settings(const std::filesystem::path& root){
    std::map<std::wstring,std::wstring> ini{{L"metrics",L"19"},{L"top",L"37"},{L"custom_key",L"keep"},{L"cpu_view_columns",L"0:0/1/14"}};
    const auto read=[&](const auto& key){return ini[key];};auto s=load_cpu_view(read);
    need(s.columns.empty() && s.package && !s.maximum,"old INI keeps existing metrics; old logical columns are not mistaken for physical cores");
    s.columns={{{1,2},true},{{0,0},false},{{0,1},true}};s.maximum=true;
    ini[L"cpu_view_core_columns"]=L"1:2/1/12,0:0/0/6,0:1/1/18";
    need(load_cpu_view(read).columns==s.columns,"legacy core entries retain their order and visibility");
    save_cpu_view(s,[&](const auto& key,const auto& value){ini[key]=value;});
    need(ini[L"cpu_view_core_columns"]==L"1:2/1,0:0/0,0:1/1","core settings store only identity and visibility");
    need(load_cpu_view(read)==s && ini[L"metrics"]==L"19" && ini[L"top"]==L"37" && ini[L"custom_key"]==L"keep","physical core settings round trip without unrelated resets");
    const auto columns=cpu_view_columns(s,topology());
    need(columns.size()==2 && columns[0].id==CpuViewId{1,2} && columns[1].id==CpuViewId{0,1},"column visibility/order identify physical cores, not representative thread numbers");
    auto absent=topology();absent.cores.pop_back();need(cpu_view_columns(s,absent).size()==1 && s.columns.size()==3,"missing core does not erase saved settings");
    ini[L"cpu_view_core_columns"]=L"0:0/1/1,0:0/1/12,1:2/1/999,bad,0:1/1/12junk";
    const auto bad=load_cpu_view(read);
    need(bad.columns==std::vector<CpuViewColumn>{{{0,0},true},{{1,2},true}},"malformed columns and duplicate core IDs are filtered");
    const auto file=(root/(L"cpu-core-settings-"+std::to_wstring(GetCurrentProcessId())+L".ini")).wstring();
    ini_overlay_write(file,L"disk_columns",5);save_cpu_view(s,[&](const auto& key,const auto& text){ini_overlay_write_text(file,key,text);});
    need(load_cpu_view([&](const auto& key){return ini_overlay_read_text(file,key);})==s && ini_overlay_read(file,L"disk_columns",0)==5,"real INI survives restart with disk settings intact");
    std::filesystem::remove(file);
}
void physical_metrics(){
    Snapshot snapshot;snapshot.cpu_topology=topology();snapshot.total_cpu_percent=20;snapshot.timestamp_ms=10000;
    snapshot.cpu_topology.sampled_tick=700;
    snapshot.cpu_logical_usage={{0,0,0,true,80},{0,1,0,true,40},{0,2,1,false,25},{1,0,2,true,std::nullopt},{1,1,2,true,10}};
    auto sample=cpu_chart_sample(snapshot,1000,10300);const auto frozen=sample;
    need(sample.temperature_time_ms==10000,"DTS timestamp excludes UI delivery delay");
    CpuViewSettings settings;settings.maximum=true;settings.average=true;
    const auto total=cpu_chart_series(settings,sample,false),temperature=cpu_chart_series(settings,sample,true);
    need(total.size()==1 && temperature.size()==3,"CPU overview keeps its existing aggregate graphs; no per-thread or all-core overlay");
    need(cpu_chart_value(total[0],sample,20)==20 && cpu_chart_value(temperature[0],sample,{})==65,"CPU aggregate and package preserved");
    CpuViewSettings unchecked;unchecked.package=false;unchecked.maximum=false;unchecked.average=false;
    const auto restored=cpu_chart_series(unchecked,sample,true);
    need(restored.size()==1 && cpu_chart_value(restored[0],sample,{})==65,
        "unchecking optional temperature metrics must not remove the CPU overview temperature chart");
    auto selected_sensor=snapshot;selected_sensor.cpu_temperature_c=49;selected_sensor.cpu_temperature_source="hwinfo";
    const auto selected_sample=cpu_chart_sample(selected_sensor);
    need(cpu_chart_value(restored[0],selected_sample,{})==49 && !selected_sample.package_from_dts,
        "overview and sidebar must show the same selected CPU temperature rather than a different DTS sensor");
    selected_sensor.cpu_temperature_c.reset();selected_sensor.cpu_topology.package_c.reset();
    need(!cpu_chart_value(restored[0],cpu_chart_sample(selected_sensor),{}),
        "missing temperature stays unavailable instead of a fabricated zero");
    const auto p=cpu_core_chart_series(sample,{0,0},false),e=cpu_core_chart_series(sample,{0,1},false),t=cpu_core_chart_series(sample,{0,1},true);
    need(p[0].title==L"P-Core 1" && e[0].title==L"E-Core 1" && t[0].title==L"E-Core 1",
        "core display names share one-based numbering within each core type without group prefixes");
    need(p.size()==1 && e.size()==1 && t.size()==1,"each core has exactly one load and one temperature series");
    need(cpu_chart_value(p[0],sample,{})==60 && cpu_chart_value(e[0],sample,{})==25,"two SMT siblings form one physical-core metric; single-thread core is not divided by two");
    need(cpu_chart_value(t[0],sample,{})==48 && t[0].unverified && t[0].title.find(L"E-Core")!=std::wstring::npos,"physical temperature and unverified E-core sensor stay distinct");
    need(!cpu_chart_value(cpu_core_chart_series(sample,{1,2},false)[0],sample,{}),"missing sibling cannot produce a fake complete core load");
    sample.logical[0].percent=0;need(cpu_chart_value(p[0],frozen,{})==60,"paused core graph keeps original aggregate");
    CpuProcessExecution app;app.status="partial";app.logical={{0,0,1000,40,40},{0,1,500,20,20},{0,2,200,15,15},{1,0,100,10,10},{1,1,200,std::nullopt,20}};
    need(cpu_application_core_load(app,{0,0},topology())==30 && cpu_application_core_load(app,{0,1},topology())==15,"app attribution combines only the logical members of its physical core");
    need(!cpu_application_core_load(app,{1,2},topology()),"unknown ETW sibling keeps physical app attribution unavailable");
    app.status="incomplete";need(!cpu_application_core_load(app,{0,0},topology()),"lost ETW trace cannot expose an exact core value");
}
void navigation(){
    for(unsigned cores:{0U,3U,24U,96U})for(unsigned disks:{0U,2U,26U}){
        need(statistics_sidebar_item_at(0,disks,cores)->index==4 && statistics_sidebar_item_at(1,disks,cores)->index==0,"AutoScore and CPU retain original sidebar positions");
        for(unsigned core=0;core<cores;++core){const auto item=statistics_sidebar_item_at(core+2,disks,cores);need(item && item->kind==StatisticsSidebarKind::core && item->index==core,"every physical core has its own sidebar entry");}
        for(unsigned section:{1U,2U,3U,5U,6U}){const auto item=statistics_sidebar_item_at(statistics_sidebar_section_row(section,disks,cores),disks,cores);
            need(item && item->kind==StatisticsSidebarKind::section && item->index==section,"existing sections remain selectable after dynamic cores and disks");}
        for(unsigned disk=0;disk<disks;++disk){const auto item=statistics_sidebar_item_at(statistics_sidebar_disk_row(disk,cores),disks,cores);need(item && item->kind==StatisticsSidebarKind::disk && item->index==disk,"disk navigation is unchanged by core count");}
    }
}
}
int wmain(int argc,wchar_t** argv){try{need(argc==2,"fixture directory required");settings(argv[1]);physical_metrics();navigation();
    std::cout<<"Physical core settings, load/temperature pairs, SMT aggregation, app attribution and navigation passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
