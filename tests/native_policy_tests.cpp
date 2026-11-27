// C++20 replacements for FluxTop source contract tests formerly written in PowerShell.
// Each contract remains a separate CTest case; no source modification is performed.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <iterator>
#include <set>
#include <sstream>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    if(!stream)throw std::runtime_error("Cannot read: "+path.generic_string());
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
bool has(const std::string& source,std::string_view text) {
    return source.find(text)!=std::string::npos;
}
void need(const std::string& source,std::initializer_list<std::string_view> fragments) {
    for(const auto fragment:fragments)
        if(!has(source,fragment))throw std::runtime_error("Missing contract: "+std::string(fragment));
}
void absent(const std::string& source,std::initializer_list<std::string_view> fragments) {
    for(const auto fragment:fragments)
        if(has(source,fragment))throw std::runtime_error("Forbidden contract: "+std::string(fragment));
}
void one_of(const std::string& source,std::initializer_list<std::string_view> fragments) {
    for(const auto fragment:fragments)if(has(source,fragment))return;
    throw std::runtime_error("None of the accepted alternatives was found");
}
void regex_need(const std::string& source,const char* pattern,bool should_match) {
    const bool matched=std::regex_search(source,std::regex(pattern));
    if(matched!=should_match)throw std::runtime_error(std::string("Regex contract failed: ")+pattern);
}
void ordered(const std::string& source,std::string_view first,std::string_view second) {
    const auto a=source.find(first),b=source.find(second);
    if(a==std::string::npos||b==std::string::npos||a>=b)
        throw std::runtime_error("Source order contract violated: "+std::string(first));
}
void check_includes(const std::filesystem::path& root,const std::filesystem::path& file,
    const std::regex& forbidden,std::set<std::filesystem::path>& visited) {
    if(!visited.insert(file).second)return;
    static const std::regex include(R"inc(^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"])inc");
    std::istringstream source(read(root/file));
    std::string line;
    std::smatch match;
    while(std::getline(source,line)) {
        if(!std::regex_search(line,match,include))continue;
        const auto dependency=match[1].str();
        if(std::regex_search(dependency,forbidden))
            throw std::runtime_error(file.generic_string()+" depends on forbidden header "+dependency);
        const auto target=dependency.starts_with("fluxtop/")?std::filesystem::path("include")/dependency:
            file.parent_path()/dependency;
        if(std::filesystem::is_regular_file(root/target))check_includes(root,target.lexically_normal(),forbidden,visited);
    }
}
void run(const std::filesystem::path& root,const std::string& policy) {
    const auto overlay=read(root/"src/overlay.cpp");
    if(policy=="architecture_boundaries") {
        const std::string collectors=R"((^|/)(sampler\.hpp|[^/]*collector\.hpp|cpu_topology_reader\.hpp|intel_dts\.hpp|amd_temperature\.hpp|pawnio_reader\.hpp|temperatures\.hpp|disk_temperature\.hpp)$)";
        const std::string platform=R"((^|/)(pdh(msg)?|evntrace|evntcons|tdh|d3dkmthk|wbemidl|winioctl|winsqlite3|sqlite3)\.h$)";
        const std::regex ui(collectors+"|"+platform,std::regex_constants::icase);
        const std::regex core(collectors+"|"+platform+R"(|(^|/)(windows|windowsx|commctrl|d2d1|dwrite)\.h$)",std::regex_constants::icase);
        std::set<std::filesystem::path> visited;
        for(const auto* file:{"include/fluxtop/model.hpp","include/fluxtop/sampling_worker.hpp",
            "src/model.cpp","src/output.cpp","src/temperature_selection.cpp"})check_includes(root,file,core,visited);
        visited.clear();
        for(const auto* file:{"src/overlay.cpp","src/statistics_page.cpp"})check_includes(root,file,ui,visited);
    } else if(policy=="update_consent_policy") {
        const auto launcher=read(root/"src/launcher.cpp");
        const auto resource=read(root/"src/launcher.rc.in");
        absent(launcher,{"if (!prepare && fluxtop::try_release_update("});
        need(launcher,{"if (check_update) {","MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2",
            "if (MessageBoxW(nullptr,question.c_str()","close_monitor_for_consented_update()"});
        need(overlay,{"check.lpParameters=L\"--check-update\";",
            "std::wstring_view(marker)==L\"FluxTop.update-consent.v1\""});
        need(resource,{"204 L\"FluxTop.update-consent.v1\""});
        ordered(launcher,"MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2","fluxtop::download_release_update(*candidate)");
        ordered(launcher,"fluxtop::download_release_update(*candidate)","if (!close_monitor_for_consented_update()");
    } else if(policy=="fps_header_policy") {
        const auto layout=read(root/"include/fluxtop/overlay_layout.hpp");
        const auto model=read(root/"src/model.cpp");
        const auto sampler=read(root/"src/sampler.cpp");
        need(layout,{"std::array<float, 5> metric_widths","std::array<const wchar_t*, 5> metric_names"});
        need(overlay,{"summary_only()?L\"FPS: ","foreground_fps_","draw_text(fps_heading,header[0]",
            "constexpr unsigned all_metrics = 31;","GetWindowThreadProcessId(active_window, &foreground_pid_)",
            "s.fps_focus_epoch == foreground_epoch_","update_view(); // Update FPS target and header in every sorting mode.",
            "else fps_heading += L\""});
        absent(overlay,{"displayed.fps","std::move(fps),loads,bursts","metric_visual_to_storage"});
        absent(model,{"max_gpu(combined.presented_fps,part.presented_fps)"});
        need(sampler,{"fps_->collect(snapshot.processes, fps_focus_)","if(!focus.pid)fps_.reset()"});
    } else if(policy=="statistics_policy") {
        const auto history=read(root/"src/history.cpp");
        const auto math=read(root/"include/fluxtop/history_math.hpp");
        const auto page=read(root/"src/statistics_page.cpp");
        const auto reader=read(root/"src/statistics_history.cpp");
        need(history,{"std::unordered_map<std::string,ApplicationMinute> apps",
            "history_floor(t,history_minute_ms)","CREATE TABLE history_rollups",
            "compact_one(db,0,1,history_hour_ms","compact_one(db,1,2,history_day_ms",
            "const auto begin=std::max(end-period,last_end_ms);","history_retention_days*history_day_ms",
            "storage_bytes(path)>=history_stop_bytes","SQLITE_CHECKPOINT_TRUNCATE",
            "wal_bytes(path)>256*mib","thread=std::thread([this]{run();})","ready.wait("});
        absent(history,{"CREATE TABLE measurements("});
        need(math,{"integral","duration_ms","correction","50ULL * 1024 * 1024 * 1024"});
        need(reader,{"SQLITE_OPEN_READONLY","sqlite3_progress_handler","FROM history_rollups",
            "statistics_is_system(","if(uptime_only || stop.stop_requested())return out;",
            "history_limit=std::clamp","uptime_limit=std::clamp"});
        need(page,{"on_snapshot","stats_show_system","request(true);return 0;",
            "history_.uptime=std::move(item->second.uptime);","DTM_SETRANGE",
            "stats_limit_id","stats_history_top","stats_uptime_top",
            "PostMessageW(GetParent(hwnd_),WM_APP+215"});
        need(overlay,{"statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top,graph_history_,icons_)",
            "statistics_->set_limit(options_.top)"});
    } else if(policy=="statistics_popout_policy") {
        const auto page=read(root/"src/statistics_page.cpp");
        const auto header=read(root/"include/fluxtop/statistics_page.hpp");
        const auto popup=read(root/"src/statistics_popout.inc");
        const auto metric=read(root/"include/fluxtop/metric_text.hpp");
        const auto ini=read(root/"include/fluxtop/overlay_ini.hpp");
        const auto win=read(root/"include/fluxtop/windows.hpp");
        need(metric,{"std::wstring decimal(double value,int precision=1)",
            "std::wstring percent(std::optional<double> value)",
            "std::wstring memory(std::optional<std::uint64_t> bytes)",
            "std::wstring network_rate(std::optional<double> rx,std::optional<double> tx)"});
        need(ini,{"ini_overlay_read(","ini_overlay_read_int(","ini_overlay_write("});
        need(win,{"utf8_to_wide(","register_window_class("});
        need(page,{"popup_auto_scale_keys", "metric_text::decimal(value)",
            "metric_text::network_rate(rx,tx)","ini_overlay_read(settings_", "ini_overlay_write(settings_"});
        need(overlay,{"using metric_text::decimal;","using metric_text::memory;",
            "using metric_text::network_rate;","utf8_to_wide(value,true)",
            "ini_overlay_read_int(settings_", "ini_overlay_write(settings_"});
        need(popup,{"popup_auto_scale_keys","ini_overlay_read(settings_", "ini_overlay_write(settings_"});
        absent(page,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        absent(overlay,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        absent(popup,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        need(header,{"PopupState","target_auto_zoom_","std::deque<PopupState> popups_{first_core_chart_kind}",
            "SystemChartPoint", "fill_enabled_", "smooth_system_end_",
            "frozen_views_", "toggle_section_pause()", "pause_toggle_"});
        need(page,{"FluxTop.SystemDashboard.VisualRefinement.v1", "draw_system_hover(dc,rect)",
            "GdiAlphaBlend(dc,left,top,right-left,bottom-top", "advance_time_view(elapsed)",
            "LPtoDP(dc,polygon.data()", "ExtSelectClipRgn(dc,mask,RGN_AND)",
            "for(const auto& plot:plots)fill_under(", "for(const auto& plot:plots) {",
            "stats_chart_fill"});
        need(read(root/"src/system_graph_history.cpp"),{"point.values[4]","points_.push_back(std::move(point))"});
        need(overlay,{"graph_history_->append(*snapshot_", "options_.top,graph_history_,icons_)"});
        need(header,{"const std::deque<SystemPoint>& system_points_"});
        need(page,{"void extend_recent_line(std::vector<POINT>& line,LONG right,",
            "void extend_fresh_left_edge(std::vector<POINT>& line,const RECT& area,",
            "if(is_live)extend_fresh_left_edge(line,area,from,to,animation_duration_ms_);",
            "extend_fresh_left_edge(points,spark,latest-60000,latest,animation_duration_ms_);",
            "eased_latest_value(double previous,double measured,ULONGLONG animation_start,",
            "extend_recent_line(line,area.right,points.back().first,to,animation_duration_ms_)",
            "extend_recent_line(line,area.right,segment_end,to,animation_duration_ms_)",
            "eased_latest_value(*previous_measurement,actual,"});
        need(page,{"void draw_time_grid(HDC dc,const RECT& area,",
            "void StatisticsPage::draw_chart_core(",
            "draw_time_grid(dc,grid_area,style.from,style.to,style.time_divisions);",
            "style.time_divisions=8;", "style.time_divisions=10;",
            "const bool sidebar_live=active_;",
            "const double current_wall_time=static_cast<double>(now_ms());",
            "const bool viewport_animating=advance_time_view(elapsed);",
            "else if(active_ && viewport_animating) {",
            "const bool sidebar_only=paint_font_ && ps.rcPaint.left>=0 &&",
            "draw_sidebar(buffer);"});
        absent(page,{"if(!live_page && !live_popup)return false;"});
        need(header,{"std::array<std::vector<double>,2> smooth_system_axis_max_",
            "const std::deque<SystemPoint>* frozen=nullptr,bool detached=false"});
        // AutoScore, system graphs and sidebar mini-charts must share the same
        // renderer instead of introducing private per-view GDI painting loops.
        std::size_t shared_calls=0;
        for(std::size_t at=0;(at=page.find("draw_chart_core(dc,style,plots",at))
            !=std::string::npos;at+=sizeof("draw_chart_core(dc,style,plots")-1)
            ++shared_calls;
        if(shared_calls!=5U)
            throw std::runtime_error("Expected five shared chart renderer call sites including physical core cards");
        need(popup,{"popup->fill_toggle", "popup_auto_id+1", "page->hover_system_charts(popup->kind",
            "frozen_system,true", "WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN",
            "if(popup->kind<2 || popup->dragging)InvalidateRect(window,nullptr,FALSE);"});
        need(header,{"PaintBuffer tooltip_buffer_;", "bool position_tooltip(POINT screen,int offset_x,int offset_y);"});
        // CPU sidebar pair (load + temperature) must use the same shared
        // renderer as the GPU/VRAM mini-chart, without polling hardware in UI.
        need(page,{"second_cpu_temperature_line",
            "if(section==0 && second_cpu_temperature_line.size()>1)",
            "plots.push_back({std::move(second_cpu_temperature_line),cpu_temperature_color})",
            "extend_fresh_left_edge(second_cpu_temperature_line",
            "draw_chart_core(dc,style,plots,bars)"});
        need(header,{"std::deque<FrozenView> frozen_views_{first_core_chart_kind}"});
        need(page,{"toggle_chart_pause(kind,frozen_views_[kind],pause_toggle_)", "refresh_pause_control();",
            "held.paused?&held.samples:nullptr", "open_popup(selected_chart_kind())",
            "const auto& info=held.paused?held.info:system_info_", "system_tip_info_.ram_capacity"});
        need(popup,{"popup_auto_scale_key(unsigned kind)", "layout_detached_controls(window,",
            "popup->auto_toggle,popup->fill_toggle,popup->pause_toggle"});
        absent(page,{"Системная метрика, не нагрузка отдельного PID"});
        need(page,{"bool StatisticsPage::position_tooltip(",
            "SWP_NOACTIVATE|SWP_NOZORDER|(visible?0U:SWP_SHOWWINDOW)",
            "page->tooltip_buffer_.prepare(dc,bounds.right,bounds.bottom)",
            "if(content_changed || newly_shown)InvalidateRect(tooltip_,nullptr,FALSE);",
            "WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN"});
        absent(page,{"SHGetFileInfoW(","graph_history_->append("});
        need(page,{"FluxTop.SystemDashboard.Stage1",
            "stats_selected_section","case WM_LBUTTONUP:","system_popout_id"});
        need(popup,{"popup.kind>=2","draw_system_charts(dc,graph,popup.kind",
            "popup_auto_scale_keys"});
        need(page,{"popup_auto_scale_keys","stats_popup_auto_cpu"});
        need(page,{"void StatisticsPage::refresh_auto_axis(unsigned index,bool immediate)",
            "refresh_auto_axis(0,false);","refresh_auto_axis(1,false);",
            "refresh_auto_axis(index,true);","if(history_axis_refit_pending_) {",
            "if(first_history_data)history_axis_refit_pending_=true;",
            "held.zoom=chart_effective_zoom(auto_scale_[index]","auto_vertical_zoom(held.apps,held.range.from,held.range.to),1.0);","history_axis_refit_pending_=true;"});
        need(page,{"popout_live_id","popout_history_id","rebuild_live_series()",
            "animation_duration_ms_","GetTickCount64()-animation_started_","IntersectClipRect",
            "stats_limit_id","stats_history_top","stats_uptime_top","animate_message",
            "SetTimer(hwnd_,animate_message,33,nullptr)","nullptr,nullptr,GetModuleHandleW(nullptr),this);",
            "target_auto_zoom_[index]","axis_initialized_[index]","popup.axis_dirty=true",
            "const auto end=kind==0?live_view_end():system_view_end();"});
        need(popup,{"WM_MOUSEWHEEL","WM_LBUTTONDOWN","WM_LBUTTONDBLCLK","draw_graph(",
            "paint_popup(","1050,820,nullptr,nullptr,GetModuleHandleW(nullptr),&popup)",
            "toggle_chart_pause(popup->kind,popup->held,popup->pause_toggle)",
            "chart_popup_window(chart_range(popup.kind,popup.held)","chart_popup_pan_limit(base,popup->zoom_x)"});
        need(overlay,{"grouped_processes_,options_.interval_ms",
            "if (wp==SIZE_MINIMIZED) return 0;"});
        absent(overlay,{"PostMessageW(window,WM_CLOSE,0,0)"});
        need(page,{"draw_system_charts(dc,selected_system_chart_area(kind)", "select_core(core_indices_[item->index]);",
            "hover_system_charts(kind,{x,y},selected_system_chart_area(kind)", "system_chart_bounds(area,panels.secondary.has_value())"});
        absent(popup,{"draw_system_graph(","chart_pair_split("});
    } else if(policy=="statistics_tooltip_axis_policy") {
        const auto page=read(root/"src/statistics_page.cpp");
        const auto header=read(root/"include/fluxtop/statistics_page.hpp");
        const auto popup=read(root/"src/statistics_popout.inc");
        need(page,{"void StatisticsPage::draw_metric_tooltip(",
            "draw_metric_tooltip(dc,bounds,source[hover_index_],hover_value_,hover_timestamp_)",
            "hover_timestamp_=sample_at","time.wSecond","local_sample_label(sample_at)",
            "const int width=MulDiv(420","const int height=MulDiv(fps_tooltip?112:198","current_readings_.emplace(color_key(identity)",
            "metric_text::network_rate(rx,tx)","L\" Mbps\"",
            "double eased_axis(double current,double target,double elapsed_ms)",
            "popup.cached_auto_zoom=eased_axis(","const bool axis_animating=advance_axes(elapsed);",
            "const bool viewport_animating=advance_time_view(elapsed);",
            "if((!animating && !axis_animating && !viewport_animating) ||",
            "(!active_ && !has_popup()))",
            "KillTimer(hwnd_,animate_message);"});
        one_of(page,{"stat_network(process->rx_bytes_per_second,process->tx_bytes_per_second)",
            "stat_network(p.rx_bytes_per_second,p.tx_bytes_per_second)"});
        need(header,{"target_auto_zoom_"});
        need(popup,{"draw_metric_tooltip(dc,bubble,apps[hit->app],hit->score,hit->sample_at)",
            "auto_score_hit(apps,graph,popup.cursor", "bubble_width*198.0/420.0",
            "popup.target_auto_zoom=auto_vertical_zoom(apps,from,to)"});
    } else if(policy=="statistics_table_popout_policy") {
        const auto page=read(root/"src/statistics_page.cpp");
        const auto header=read(root/"include/fluxtop/statistics_page.hpp");
        const auto reader=read(root/"src/statistics_history.cpp");
        need(page,{"popout_table_id","void StatisticsPage::open_table()",
            "void StatisticsPage::populate_table()","fill(table_,image_list_)",
            "fill(table_window_list_,table_image_list_)","&& !table_window_) return;",
            "sorted.size(),100U","StatisticsPage::hover_dispatch",
            "WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE",
            "case WM_NCHITTEST:return HTTRANSPARENT;",
            "put(limit_label_,292,66,260,28);","put(history_limit_label_,292,65,230,28);",
            "put(period_,292,111,158,200);","put(uptime_limit_label_,292,66,205,28);",
            "put(show_system_,722,64,229,31);","request(true);return 0;",
            "if(uptime_only) {","history_.uptime=std::move(item->second.uptime);",
            "if(statistics_is_system(identity.name,identity.path)) continue;",
            "put(uptime_period_,292,111,158,190);",
            "uptime_period_id && HIWORD(wp)==CBN_SELCHANGE",
            "requested_={from,to,uptime_from,uptime_to,",
            "const auto [uptime_from,uptime_to]=range(uptime_period_index_,uptime_from_,uptime_to_);"});
        absent(page,{"put(show_system_,412,5,326,30);"});
        need(header,{"uptime_from{},uptime_to{}","HWND table_window_","HWND tooltip_"});
        need(reader,{"history_limit,false,true,stop);","live_limit,false,true,stop);",
            "out.uptime=top_apps(connection.db,uptime_from_ms,uptime_to_ms,"});
    } else if(policy=="gpu_group_memory_policy") {
        const auto collector=read(root/"src/gpu_collector.cpp");
        const auto model=read(root/"src/model.cpp");
        const auto history=read(root/"src/history.cpp");
        need(collector,{"if (row.dedicated_gpu_bytes && !gpu_process_memory_consistent("});
        need(model,{"add_bytes(combined.dedicated_gpu_bytes,part.dedicated_gpu_bytes);"});
        absent(overlay,{"gpu_process_memory_consistent("});
        absent(history,{"gpu_process_memory_consistent("});
    } else if(policy=="overlay_animation_policy") {
        need(overlay,{"bool animation_enabled_ = true;","read(L\"overlay_animation\", 1)",
            "save(L\"overlay_animation\", animation_enabled_ ? 1 : 0)",
            "animation_control, 22, 704, 536, 26","case animation_control:",
            "if(pinned) from=i==0?0.0f:std::max(1.0f,from);",
            "target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,table.total_width,",
            "constexpr UINT overlay_animation_timer_ms = 40;",
            "SetTimer(hwnd_,overlay_animation_timer,overlay_animation_timer_ms,nullptr)",
            "order_changed","struct CachedRowTextLayouts {","cache_capacity = std::clamp",
            "DrawTextLayout(","row_text_cache_.clear();","draw_cached_row_text(row,0,row.name",
            "draw_cached_row_text(row,static_cast<unsigned>(metric+1)","row_animation_active_=has_motion;",
            "if(has_motion) {","previous_rows_.clear();","largest_pixel_shift>=0.85f",
            "row_animation_last_painted_progress_","paint(true)","if(!animation_only) {",
            "target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,",
            "A colored CPU/GPU heading has two ranges, but only needs one text layout.",
            "const auto table=table_geometry(","if (!animation_enabled_) {",
            "const bool finished=!row_animation_active_ || elapsed>=row_animation_duration_ms_;",
            "if(finished) {","KillTimer(hwnd_,overlay_animation_timer);",
            "const float opacity=1.0f; // Never fade newly appearing rows or their icons.",
            "overlay_transition_duration_ms(max_slot_travel,options_.interval_ms)",
            "overlay_transition_budget_ms(options_.interval_ms)","frame_cost+8ULL,40ULL,160ULL",
            "row_image_budget_bytes = 6U*1024U*1024U","max_builds_per_frame = 6",
            "clear_row_images(); // No animation textures or extra RAM in idle.",
            "row_images_.clear();","CreateCompatibleRenderTarget(",
            "offscreen->GetBitmap(&bitmap)","target_->DrawBitmap(row_bitmap,",
            "draw_base_row(row,geometry,0,offscreen.Get(),ink.Get())","draw_base_row(row,geometry,y)",
            "header_colors_?reading:std::nullopt","row_colors_?reading:std::nullopt"});
        absent(overlay,{"paint_values(*previous,1.0f-progress)",
            "auto_metric_has_burst(p,static_cast<AutoMetric>(j+1))",
            "true,true,displayed.loads[metric],alpha","options_.interval_ms*0.65",
            "frame_cost*2"});
    } else if(policy=="overlay_settings_animation_policy") {
        absent(overlay,{"CpuSettingsPage","TabCtrl_InsertItem(tabs_,2"});
        need(overlay,{"cpu_checkbox_control+static_cast<UINT>(ordinal)","BS_AUTOCHECKBOX|WS_TABSTOP,cpu_checkbox_control", "cpu_application_core_load(p.cpu_execution"});
        absent(overlay,{"cpu_list_=control(WC_LISTVIEWW","cpu_up_control","cpu_width_control"});
        need(overlay,{"font_control, 290, 146, 268, 32","MAKELPARAM(5,42)",
            "font_control,TBM_GETPOS","interval_control, 290, 378, 268, 32",
            "interval_step_ms = 500","interval_last_step = 10000 / interval_step_ms",
            "MAKELPARAM(interval_first_step,interval_last_step)","interval_control,TBM_GETPOS","save(L\"font_size\"",
            "save(L\"interval_ms\"","const bool is_font=",
            "SetTimer(window,settings_timer,150,nullptr)","apply_client();",
            "statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top,graph_history_,icons_)",
            "if(page!=0 && client_page_==0) {",
            "const auto pending_top=SendDlgItemMessageW(client_,top_control,TBM_GETPOS,0,0);",
            "static_cast<unsigned>(pending_top)!=options_.top) {",
            "if(statistics_) statistics_->set_limit(options_.top);",
            "std::unordered_multimap<std::uint32_t,std::size_t> previous_by_pid",
            "previous_by_pid.equal_range(rows_[i].pid)",
            "first_visible=std::min(scroll_,row_motion_.size())",
            "last_visible=std::min(row_motion_.size(),first_visible+visible_count)",
            "row_animation_next_frame_tick_","overlay.animation_slow",
            "frame_cost+8ULL,40ULL,160ULL","bool partial_body=false;",
            "if(partial_body && (y+row_height()+2<dirty_top || y-2>dirty_bottom))continue;",
            "const float opacity=1.0f; // Never fade newly appearing rows or their icons.",
            "sort_ui_values{Sort::automatic, Sort::cpu, Sort::ram, Sort::gpu",
            "sort_ui_index(options_.sort)",
            "sort_ui_values[static_cast<std::size_t>(selected_sort)]",
            "new_sort != Sort::gpu && new_sort != Sort::vram"});
        absent(overlay,{"CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())",
            "GetDlgItemInt(client_, interval_control"});
        need(overlay,{"for (const auto label : {L\"Авто\", L\"CPU\", L\"RAM\", L\"GPU\""});
        regex_need(overlay,"L\"Авто\\s*[—-]",false);
        need(overlay,{"disk_checkbox_control = 300","std::array<HWND,26> disk_checks_{};",
            "disk_checkbox_control+ordinal","const int cpu_y=486+rows*32;","const int summary_y=cpu_aggregate_y+36;",
            "open_group_==group","edit_overlay_scene(window,hwnd_","edit_overlay_blacklist(window,learned_apps_",
            "manual_move_control,22,81","manual_movement_?HTCAPTION:HTTRANSPARENT",
            "if(dragging_)return;","overlay_position_from_rect(bounds,","scene_position_changed",
            "position(cpu_temperature_label_,temperature_y)","const int cpu_aggregate_y=temperature_y+heading_spacing;",
            "SendMessageW(disk_checks_[i],BM_GETCHECK,0,0)",
            "right_anchor_ = read(L\"anchor_right\"", "save(L\"anchor_right\"",
            "overlay_fitted_left(right, width, area.left, area.right, margin)"});
        absent(overlay,{"LBS_MULTIPLESEL","LB_GETSEL","LB_SETSEL",
            "QueryFullProcessImageNameW(","OpenProcess(",
            "Диски (% активности всей системы, не по приложениям):"});
        const auto disk_page=read(root/"src/statistics_page.cpp");
        absent(disk_page,{"общая нагрузка, не отдельного процесса"});
    } else if(policy=="source_bootstrap_policy") {
        const auto auto_start=read(root/"src/autostart.cpp");
        const auto core=read(root/"src/source_checkout.cpp");
        const auto cli=read(root/"src/source_launcher.cpp");
        const auto cmake=read(root/"CMakeLists.txt");
        const auto workflow=read(root/".github/workflows/ci.yml");
        need(auto_start,{"FluxTopSourceAutostart.exe","migrate_autostart_if_needed()",
                         "StartupApproved","owned(link.Get(),path)","SetShowCmd(SW_HIDE)"});
        absent(auto_start,{"Start-FluxTop.vbs","L\"//B //Nologo"});
        need(core,{"local-changes","detached-head","operation-in-progress",
                   "local-commits-or-diverged","fetch-failed","merge-base","--ff-only",
                   "last-good.json","FILE_FLAG_DELETE_ON_CLOSE"});
        need(cli,{"CreateFileW(lock.c_str()","--prepare-only","--standard-user",
                  "L\"runas\"","--autostart","--migrate-autostart-only"});
        need(cmake,{"fluxtop_source_checkout_tests","fluxtop_source_autostart"});
        absent(workflow,{"update_tests.ps1","launcher_tests.ps1","elevation_tests.ps1"});
    } else {
        throw std::runtime_error("Unknown policy: "+policy);
    }
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) {std::cerr<<"usage: native_policy_tests <root> <policy>\n";return 2;}
    try {
        std::string policy;
        for(const wchar_t* p=argv[2];*p;++p)policy.push_back(static_cast<char>(*p));
        run(std::filesystem::path(argv[1]),policy);
        std::cout<<policy<<" passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"Contract failed: "<<e.what()<<'\n';
        return 1;
    }
}
