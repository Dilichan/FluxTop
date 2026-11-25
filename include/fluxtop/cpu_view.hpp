#pragma once
#include "fluxtop/model.hpp"
#include <functional>
#include <string_view>
#include <span>

namespace fluxtop {
struct CpuViewId {
    unsigned group{},number{};
    bool operator==(const CpuViewId&)const=default;
};
struct CpuViewColumn {
    CpuViewId id;
    bool visible{};
    bool operator==(const CpuViewColumn&)const=default;
};
struct CpuViewSettings {
    static constexpr unsigned column_limit=512;
    std::vector<CpuViewColumn> columns; // Stable order, including temporarily absent CPUs.
    bool package=true,maximum=false,average=false;
    bool operator==(const CpuViewSettings&)const=default;
};
using CpuSettingRead=std::function<std::wstring(const std::wstring&)>;
using CpuSettingWrite=std::function<void(const std::wstring&,const std::wstring&)>;
CpuViewSettings load_cpu_view(const CpuSettingRead& read);
void save_cpu_view(const CpuViewSettings& settings,const CpuSettingWrite& write);
std::vector<CpuViewColumn> cpu_view_columns(const CpuViewSettings& settings,const CpuTopologySnapshot& topology);
std::optional<double> cpu_application_load(const CpuProcessExecution& execution,CpuViewId id);
std::optional<double> cpu_application_core_load(const CpuProcessExecution& execution,CpuViewId id,const CpuTopologySnapshot& topology);
std::wstring cpu_core_label(const CpuCoreTemperature& core,std::span<const CpuCoreTemperature> topology);
struct CpuChartSample {
    std::wstring name;
    std::vector<CpuLogicalUsage> logical;
    std::vector<CpuCoreTemperature> cores;
    std::optional<double> package,maximum,average;
    std::optional<std::int64_t> temperature_time_ms;
    bool package_from_dts{};
    bool operator==(const CpuChartSample&)const=default;
};
enum class CpuSeriesKind { total,package,maximum,average,core,core_load };
struct CpuChartSeries {
    CpuSeriesKind kind{};
    CpuViewId id{};
    std::wstring title;
    bool unverified{};
};
CpuChartSample cpu_chart_sample(const Snapshot& snapshot,std::uint64_t collection_tick=0,
    std::optional<std::int64_t> collection_time_ms={});
std::vector<CpuChartSeries> cpu_chart_series(const CpuViewSettings& settings,
    const CpuChartSample& sample,bool temperature);
std::vector<CpuChartSeries> cpu_core_chart_series(const CpuChartSample& sample,CpuViewId core,bool temperature);
std::optional<double> cpu_chart_value(const CpuChartSeries& series,const CpuChartSample& sample,
    std::optional<double> total);
}
