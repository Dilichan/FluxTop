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
void run(const std::filesystem::path& root,const std::string& policy) {
    const auto overlay=read(root/"src/overlay.cpp");
    if(policy=="update_consent_policy") {
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
        const auto collector=read(root/"src/fps_collector.cpp");
        const auto model=read(root/"src/model.cpp");
        const auto sampler=read(root/"src/sampler.cpp");
        need(layout,{"std::array<float, 5> metric_widths","std::array<const wchar_t*, 5> metric_names"});
        need(overlay,{"std::wstring title = L\"","foreground_fps_","draw_text(title,header[0]",
            "constexpr unsigned all_metrics = 31;","GetWindowThreadProcessId(active_window, &foreground_pid_)",
            "fps_targets.push_back(foreground_pid_)","update_view(); // Update FPS target and header in every sorting mode.",
            "else title += L\""});
        absent(overlay,{"displayed.fps","std::move(fps),loads,bursts","metric_visual_to_storage"});
        absent(model,{"max_gpu(combined.presented_fps,part.presented_fps)"});
        need(collector,{"status != 0 && status != 7","status == 6","PresentMonAPI2Loader.dll",
            "std::optional<double> best;"});
        absent(collector,{"status != 0 && status != 8"});
        need(sampler,{"fps_.collect(snapshot.processes, fps_targets_)"});
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
        need(overlay,{"statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top)",
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
        need(popup,{"popup_auto_scale_keys","ini_overlay_read(settings_", "ini_overlay_write(page->settings_"});
        absent(page,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        absent(overlay,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        absent(popup,{"GetPrivateProfileIntW(","WritePrivateProfileStringW(","RegisterClassExW("});
        need(header,{"PopupState","target_auto_zoom_","std::array<PopupState,32> popups_",
            "std::array<std::optional<double>,5> values", "fill_enabled_", "smooth_system_end_",
            "frozen_views_", "toggle_section_pause()", "pause_toggle_"});
        need(page,{"FluxTop.SystemDashboard.VisualRefinement.v1", "draw_system_hover(dc,rect)",
            "GdiAlphaBlend(dc,left,top,right-left,bottom-top", "point.values[4]", "advance_time_view(elapsed)",
            "LPtoDP(dc,polygon.data()", "ExtSelectClipRgn(dc,mask,RGN_AND)",
            "for(const auto& plot:plots)fill_under(", "for(const auto& plot:plots) {",
            "stats_chart_fill"});
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
            "const bool live_page=active_ && selected_section_<5",
            "const double current_wall_time=static_cast<double>(now_ms());",
            "const bool viewport_animating=advance_time_view(elapsed);"});
        need(header,{"std::array<std::array<double,31>,2> smooth_system_axis_max_",
            "const std::deque<SystemPoint>* frozen=nullptr,bool detached=false"});
        // AutoScore, system graphs and sidebar mini-charts must share the same
        // renderer instead of introducing private per-view GDI painting loops.
        std::size_t shared_calls=0;
        for(std::size_t at=0;(at=page.find("draw_chart_core(dc,style,plots",at))
            !=std::string::npos;at+=sizeof("draw_chart_core(dc,style,plots")-1)
            ++shared_calls;
        if(shared_calls!=4U)
            throw std::runtime_error("Expected exactly four shared chart renderer call sites");
        need(popup,{"popup->fill_toggle", "popup_auto_id+1", "show_system_hover(popup->kind-2",
            "frozen_system,true", "WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN",
            "if(popup->kind<2 || popup->dragging)InvalidateRect(window,nullptr,FALSE);"});
        need(header,{"PaintBuffer tooltip_buffer_;", "bool position_tooltip(POINT screen,int offset_x,int offset_y);"});
        need(header,{"std::array<FrozenView,32> frozen_views_"});
        need(page,{"const bool disk=selected_disk_letter_!=0;", "refresh_pause_control();",
            "held.paused?&held.samples:nullptr", "open_popup(selected_disk_letter_"});
        need(popup,{"popup_auto_scale_key(unsigned kind)", "popup.kind>=6?popup.kind-1:popup.kind-2",
            "popup->kind>=6?popup->kind-1:popup->kind-2"});
        absent(page,{"Системная метрика, не нагрузка отдельного PID"});
        need(page,{"bool StatisticsPage::position_tooltip(",
            "SWP_NOACTIVATE|SWP_NOZORDER|(visible?0U:SWP_SHOWWINDOW)",
            "page->tooltip_buffer_.prepare(dc,bounds.right,bounds.bottom)",
            "if(content_changed || newly_shown)InvalidateRect(tooltip_,nullptr,FALSE);",
            "WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN"});
        need(page,{"FluxTop.SystemDashboard.Stage1","system_points_.push_back(point)",
            "stats_selected_section","case WM_LBUTTONUP:","system_popout_id"});
        need(popup,{"popup.kind>=2","draw_system_graph(dc,graph,popup.kind-2",
            "popup_auto_scale_keys"});
        need(page,{"popup_auto_scale_keys","stats_popup_auto_cpu"});
        need(page,{"popout_live_id","popout_history_id","rebuild_live_series()",
            "animation_duration_ms_","GetTickCount64()-animation_started_","IntersectClipRect",
            "stats_limit_id","stats_history_top","stats_uptime_top","animate_message",
            "SetTimer(hwnd_,animate_message,33,nullptr)","nullptr,nullptr,GetModuleHandleW(nullptr),this);",
            "target_auto_zoom_[index]","axis_initialized_[index]","popup.axis_dirty=true",
            "const auto now=live_view_end();"});
        need(popup,{"WM_MOUSEWHEEL","WM_LBUTTONDOWN","WM_LBUTTONDBLCLK","draw_graph(",
            "paint_popup(","1050,590,nullptr,nullptr,GetModuleHandleW(nullptr),&popup)",
            "popup.kind==0?live_view_end():window_to_"});
        need(overlay,{"grouped_processes_,options_.interval_ms",
            "if (wp==SIZE_MINIMIZED) return 0;"});
        absent(overlay,{"PostMessageW(window,WM_CLOSE,0,0)"});
    } else if(policy=="statistics_tooltip_axis_policy") {
        const auto page=read(root/"src/statistics_page.cpp");
        const auto header=read(root/"include/fluxtop/statistics_page.hpp");
        const auto popup=read(root/"src/statistics_popout.inc");
        need(page,{"void StatisticsPage::draw_metric_tooltip(",
            "draw_metric_tooltip(dc,bounds,source[hover_index_],hover_value_,hover_timestamp_)",
            "hover_timestamp_=sample_at","time.wSecond","local_sample_label(sample_at)",
            "const int width=MulDiv(420","const int height=MulDiv(198","current_readings_.emplace(color_key(identity)",
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
        need(popup,{"draw_metric_tooltip(dc,bubble,app,score,sample_at)",
            "sample_at=pts[j].first","bubble_width*198.0/420.0",
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
    } else if(policy=="overlay_animation_policy") {
        need(overlay,{"bool animation_enabled_ = true;","read(L\"overlay_animation\", 1)",
            "save(L\"overlay_animation\", animation_enabled_ ? 1 : 0)",
            "animation_control, 22, 704, 536, 26","case animation_control:",
            "if(pinned) from=i==0?0.0f:std::max(1.0f,from);",
            "target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,table_width(),",
            "constexpr UINT overlay_animation_timer_ms = 40;",
            "SetTimer(hwnd_,overlay_animation_timer,overlay_animation_timer_ms,nullptr)",
            "order_changed","struct CachedRowTextLayouts {","cache_capacity = std::clamp",
            "DrawTextLayout(","row_text_cache_.clear();","draw_cached_row_text(row,0,row.name",
            "draw_cached_row_text(row,static_cast<unsigned>(metric+1)","row_animation_active_=has_motion;",
            "if(has_motion) {","previous_rows_.clear();","largest_pixel_shift>=0.85f",
            "row_animation_last_painted_progress_","paint(true)","if(!animation_only) {",
            "target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,",
            "A colored CPU/GPU heading has two ranges, but only needs one text layout.",
            "const auto geometry=calculate_overlay_columns(","if (!animation_enabled_) {",
            "const bool finished=!row_animation_active_ || elapsed>=row_animation_duration_ms_;",
            "if(finished) {","KillTimer(hwnd_,overlay_animation_timer);",
            "const float opacity=1.0f; // Never fade newly appearing rows or their icons.",
            "overlay_transition_duration_ms(max_slot_travel)","frame_cost+8ULL,40ULL,160ULL",
            "row_image_budget_bytes = 6U*1024U*1024U","max_builds_per_frame = 6",
            "clear_row_images(); // No animation textures or extra RAM in idle.",
            "row_images_.clear();","CreateCompatibleRenderTarget(",
            "offscreen->GetBitmap(&bitmap)","target_->DrawBitmap(row_bitmap,"});
        absent(overlay,{"paint_values(*previous,1.0f-progress)",
            "auto_metric_has_burst(p,static_cast<AutoMetric>(j+1))",
            "true,true,displayed.loads[metric],alpha","options_.interval_ms*0.65",
            "frame_cost*2"});
    } else if(policy=="overlay_settings_animation_policy") {
        need(overlay,{"font_control, 290, 146, 268, 32","MAKELPARAM(5,42)",
            "font_control,TBM_GETPOS","interval_control, 290, 378, 268, 32",
            "MAKELPARAM(1,10)","interval_control,TBM_GETPOS","save(L\"font_size\"",
            "save(L\"interval_ms\"","const bool is_font=",
            "SetTimer(window,settings_timer,150,nullptr)","apply_client();",
            "statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top)",
            "if(page==1 && client_page_!=1) {",
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
            "disk_checkbox_control+ordinal","const int summary_y=486+rows*32;",
            "SendMessageW(disk_checks_[i],BM_GETCHECK,0,0)",
            "const int preferred_x=static_cast<int>(rect.right)-width;"});
        absent(overlay,{"LBS_MULTIPLESEL","LB_GETSEL","LB_SETSEL",
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
