#include "fluxtop/cpu_view.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>

namespace fluxtop {
namespace {
std::wstring key(CpuViewId id){return std::to_wstring(id.group)+L":"+std::to_wstring(id.number);}
std::optional<unsigned> integer(std::wstring_view text) {
    if(text.empty() || text.size()>5)return {};
    unsigned value{};
    for(const auto c:text){if(c<L'0' || c>L'9')return {};value=value*10+static_cast<unsigned>(c-L'0');}
    return value;
}
std::optional<CpuViewId> identity(std::wstring_view text) {
    const auto colon=text.find(L':');if(colon==text.npos)return {};
    const auto group=integer(text.substr(0,colon)),number=integer(text.substr(colon+1));
    if(!group || !number || *group>65535 || *number>65535)return {};
    return CpuViewId{*group,*number};
}
std::optional<double> valid(std::optional<double> value) {
    return value && std::isfinite(*value) && *value>=0?value:std::nullopt;
}
}
CpuViewSettings load_cpu_view(const CpuSettingRead& read) {
    CpuViewSettings settings;
    settings.package=read(L"cpu_view_package")!=L"0";
    settings.maximum=read(L"cpu_view_maximum")==L"1";settings.average=read(L"cpu_view_average")==L"1";
    std::wistringstream input(read(L"cpu_view_core_columns"));std::wstring item;unsigned visible{};
    while(settings.columns.size()<512 && std::getline(input,item,L',')) {
        const auto a=item.find(L'/'),b=item.find(L'/',a==item.npos?item.size():a+1);
        if(a==item.npos || b==item.npos)continue;
        const auto id=identity(std::wstring_view(item).substr(0,a));
        const auto width=integer(std::wstring_view(item).substr(b+1));
        if(!id || !width || (item.substr(a+1,b-a-1)!=L"0" && item.substr(a+1,b-a-1)!=L"1"))continue;
        if(std::any_of(settings.columns.begin(),settings.columns.end(),[&](const auto& column){return column.id==*id;}))continue;
        const bool show=item[a+1]==L'1' && visible<CpuViewSettings::column_limit;
        if(show)++visible;
        settings.columns.push_back({*id,show,std::clamp(*width,6U,18U)});
    }
    return settings;
}
void save_cpu_view(const CpuViewSettings& settings,const CpuSettingWrite& write) {
    write(L"cpu_view_package",settings.package?L"1":L"0");write(L"cpu_view_maximum",settings.maximum?L"1":L"0");
    write(L"cpu_view_average",settings.average?L"1":L"0");
    std::wstring text;for(const auto& column:settings.columns){
        if(!text.empty())text+=L",";
        text+=key(column.id)+L"/"+(column.visible?L"1":L"0")+L"/"+std::to_wstring(column.width);
    }
    write(L"cpu_view_core_columns",text);
}
std::vector<CpuViewColumn> cpu_view_columns(const CpuViewSettings& settings,const IntelDtsSnapshot& topology){
    std::vector<CpuViewColumn> result;
    for(const auto& column:settings.columns)if(column.visible && result.size()<CpuViewSettings::column_limit &&
        std::any_of(topology.cores.begin(),topology.cores.end(),[&](const auto& cpu){return column.id==CpuViewId{cpu.group,cpu.core_id};}))result.push_back(column);
    return result;
}
std::optional<double> cpu_application_load(const CpuProcessExecution& execution,CpuViewId id) {
    if(execution.status!="ready" && execution.status!="partial")return {};
    for(const auto& cpu:execution.logical)if(id==CpuViewId{cpu.group,cpu.number})return cpu_logical_percent(cpu.percent.value_or(-1));
    return {};
}
std::vector<std::pair<float,float>> cpu_column_layout(float first,float font,float gap,std::span<const CpuViewColumn> columns){
    std::vector<std::pair<float,float>> result;result.reserve(columns.size());
    for(const auto& column:columns){const float right=first+font*static_cast<float>(column.width);
        result.emplace_back(first,right);first=right+font*gap;}
    return result;
}
std::wstring cpu_core_label(const IntelCoreTemperature& core,std::span<const IntelCoreTemperature> topology){
    unsigned ordinal=0;
    for(const auto& entry:topology){
        if(entry.performance==core.performance)++ordinal;
        if(entry.group==core.group && entry.core_id==core.core_id)break;
    }
    return std::wstring(core.performance?L"P-Core ":L"E-Core ")+std::to_wstring(ordinal);
}
CpuChartSample cpu_chart_sample(const Snapshot& snapshot,std::uint64_t collection_tick,std::optional<std::int64_t> collection_time_ms){
    CpuChartSample sample;const auto& cores=snapshot.intel_core_temperatures;
    sample.name=cores.cpu_name;sample.logical=snapshot.cpu_logical_usage;sample.cores=cores.cores;
    // Use the same selected CPU sensor as the sidebar and system details.
    sample.package=snapshot.cpu_temperature_c?snapshot.cpu_temperature_c:cores.package_c;
    sample.package_from_dts=snapshot.cpu_temperature_c?snapshot.cpu_temperature_source=="intel-dts-pawnio":cores.package_c.has_value();
    if(cores.sampled_tick && collection_tick>=cores.sampled_tick && collection_tick-cores.sampled_tick<=30000)
        sample.temperature_time_ms=collection_time_ms.value_or(snapshot.timestamp_ms)-static_cast<std::int64_t>(collection_tick-cores.sampled_tick);
    sample.maximum=cores.core_max_c;sample.average=cores.core_average_c;return sample;
}
std::vector<CpuChartSeries> cpu_chart_series(const CpuViewSettings& settings,const CpuChartSample& sample,bool temperature){
    (void)sample;
    std::vector<CpuChartSeries> result;
    if(!temperature) {
        result.push_back({CpuSeriesKind::total,{},L"CPU",false});
    }else {
        // The overview's existing temperature chart must not disappear when
        // every optional temperature display switch is unchecked.
        if(settings.package || (!settings.maximum && !settings.average))
            result.push_back({CpuSeriesKind::package,{},L"Температура ЦП",false});
        if(settings.maximum)result.push_back({CpuSeriesKind::maximum,{},L"Core Max",false});
        if(settings.average)result.push_back({CpuSeriesKind::average,{},L"Core Average",false});
    }
    return result;
}
std::vector<CpuChartSeries> cpu_core_chart_series(const CpuChartSample& sample,CpuViewId id,bool temperature){
    std::vector<CpuChartSeries> result;
    const auto core=std::find_if(sample.cores.begin(),sample.cores.end(),[&](const auto& item){return id==CpuViewId{item.group,item.core_id};});
    if(core==sample.cores.end())return result;
    if(temperature)result.push_back({CpuSeriesKind::core,id,cpu_core_label(*core,sample.cores),core->sensor_independent_unverified});
    else {
        result.push_back({CpuSeriesKind::core_load,id,cpu_core_label(*core,sample.cores),false});
    }
    return result;
}
std::optional<double> cpu_application_core_load(const CpuProcessExecution& execution,CpuViewId id,const IntelDtsSnapshot& topology){
    unsigned expected{};for(const auto& core:topology.cores)if(id==CpuViewId{core.group,core.core_id})expected=core.logical_siblings;
    double sum=0;unsigned count=0;
    for(const auto& cpu:topology.logical_members)if(cpu.group==id.group && cpu.core_id==id.number){
        const auto value=cpu_application_load(execution,{cpu.group,cpu.number});if(!value)return {};sum+=*value;++count;
    }
    return count && count==expected?std::optional<double>{sum/count}:std::nullopt;
}
std::optional<double> cpu_chart_value(const CpuChartSeries& series,const CpuChartSample& sample,std::optional<double> total){
    switch(series.kind){
    case CpuSeriesKind::total:return cpu_logical_percent(total.value_or(-1));
    case CpuSeriesKind::package:return valid(sample.package);
    case CpuSeriesKind::maximum:return valid(sample.maximum);
    case CpuSeriesKind::average:return valid(sample.average);
    case CpuSeriesKind::core:
        for(const auto& core:sample.cores)if(series.id==CpuViewId{core.group,core.core_id})return valid(core.celsius);break;
    case CpuSeriesKind::core_load: {
        unsigned expected{};for(const auto& core:sample.cores)if(series.id==CpuViewId{core.group,core.core_id})expected=core.logical_siblings;
        double sum=0;unsigned count=0;
        for(const auto& cpu:sample.logical)if(cpu.group==series.id.group && cpu.core_id==series.id.number){
            const auto value=cpu_logical_percent(cpu.percent.value_or(-1));if(!value)return {};sum+=*value;++count;
        }
        return count && count==expected?std::optional<double>{sum/count}:std::nullopt;
    }
    }
    return {};
}
}
