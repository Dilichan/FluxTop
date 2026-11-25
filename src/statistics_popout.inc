// Reuse the StatisticsPage model and colour map: detached windows never collect data.
// The original six popup indices are stable; disk A:..Z: use kinds 6..31.
namespace {
RECT detached_plot(const DetachedChartLayout& layout) {
    const auto& plot=layout.plot;
    return {plot.left,plot.top,plot.right,plot.bottom};
}
DetachedChartLayout detached_layout(HWND window) {
    RECT client{};GetClientRect(window,&client);
    return chart_detached_layout(client.right,client.bottom);
}
void layout_detached_controls(HWND window,const std::array<HWND,3>& controls) {
    const auto layout=detached_layout(window);
    for(std::size_t index=0;index<controls.size();++index) {
        const auto& box=layout.controls[index];
        MoveWindow(controls[index],box.left,box.top,box.right-box.left,box.bottom-box.top,TRUE);
    }
}
}
std::wstring popup_auto_scale_key(unsigned kind) {
    if(kind>=32)return L"stats_popup_auto_core_"+std::to_wstring(kind-32);
    if(kind<popup_auto_scale_keys.size())return popup_auto_scale_keys[kind];
    return L"stats_popup_auto_disk_"+std::wstring(1,static_cast<wchar_t>(L'A'+kind-6));
}
ChartTimeWindow StatisticsPage::popup_range(const PopupState& popup) const {
    return chart_popup_window(chart_range(popup.kind,popup.held),popup.zoom_x,popup.pan_ms);
}
void StatisticsPage::set_popup_autoscale(PopupState& popup,bool enabled) {
    popup.auto_scale=enabled;
    popup.axis_dirty=true;
    SendMessageW(popup.auto_toggle,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
    const auto key=popup_auto_scale_key(popup.kind);
    ini_overlay_write(settings_,key.c_str(),enabled);
    InvalidateRect(popup.window,nullptr,FALSE);
}
void StatisticsPage::open_popup(unsigned kind) {
    if(kind>=popups_.size()) return;
    auto& popup=popups_[kind];
    if(popup.window && IsWindow(popup.window)) {
        ShowWindow(popup.window,SW_RESTORE);
        SetForegroundWindow(popup.window);
        return;
    }
    constexpr wchar_t window_class[]=L"FluxTop.Statistics.DetachedGraph.v1";
    WNDCLASSEXW cls{};
    cls.cbSize=sizeof(cls);
    cls.style=CS_DBLCLKS|CS_HREDRAW|CS_VREDRAW;
    cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpfnWndProc=popup_dispatch;
    cls.lpszClassName=window_class;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    register_window_class(cls,"Register detached statistics graph");
    popup.owner=this;popup.kind=kind;
    popup.zoom_x=1.0;popup.zoom_y=1.0;popup.pan_ms=0;
    popup.cached_auto_zoom=1.0;popup.target_auto_zoom=1.0;
    popup.axis_initialized=false;popup.axis_dirty=true;
    const auto auto_key=popup_auto_scale_key(kind);
    popup.auto_scale=ini_overlay_read(settings_,auto_key.c_str(),1)!=0;
    popup.dragging=false;popup.cursor={-1,-1};
    const std::array<const wchar_t*,6> titles{{L"FluxTop — последняя минута (AutoScore)",
        L"FluxTop — история (AutoScore)",L"FluxTop — ЦП",L"FluxTop — память",
        L"FluxTop — сеть",L"FluxTop — GPU"}};
    const auto disk_title=kind>=32?L"FluxTop — "+cpu_core_label(core_entries_[kind-32],core_entries_):kind>=6
        ?L"FluxTop — Disk "+std::wstring(1,static_cast<wchar_t>(L'A'+kind-6))
        :std::wstring{};
    const wchar_t* title=kind<titles.size()?titles[kind]:disk_title.c_str();
    popup.window=CreateWindowExW(0,window_class,title,
        WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,1050,820,nullptr,nullptr,GetModuleHandleW(nullptr),&popup);
    if(!popup.window) throw windows_error("Create detached statistics graph");
    ShowWindow(popup.window,SW_SHOW);
    UpdateWindow(popup.window);
    // The historical popout keeps refreshing even when the Settings tab is active.
    if(kind==1) request();
}
void StatisticsPage::paint_popup(PopupState& popup,HDC dc) {
    RECT client{};GetClientRect(popup.window,&client);
    // Native background and labels; the graph retains its hand-drawn look.
    FillRect(dc,&client,GetSysColorBrush(COLOR_BTNFACE));
    const auto layout=chart_detached_layout(client.right,client.bottom);
    const auto graph=detached_plot(layout);
    if(graph.right<=graph.left || graph.bottom<=graph.top) return;
    const auto old_font=SelectObject(dc,chart_font());
    const std::array<const wchar_t*,6> names{{L"AutoScore · последняя минута",L"AutoScore · история",
        L"ЦП",L"Память",L"Сеть",L"GPU"}};
    const auto disk_name=popup.kind>=32?cpu_core_label(core_entries_[popup.kind-32],core_entries_):popup.kind>=6
        ?L"Disk "+std::wstring(1,static_cast<wchar_t>(L'A'+popup.kind-6))
        :std::wstring{};
    label(dc,popup.kind<names.size()?names[popup.kind]:disk_name.c_str(),22,layout.title_top);
    // Controls are native Win32; navigation instructions are intentionally absent.
    const auto [from,to]=popup_range(popup);
    const auto* frozen_system=popup.held.paused?&popup.held.samples:nullptr;
    if(popup.kind>=2) {
        draw_system_charts(dc,graph,popup.kind,from,to,popup.auto_scale,popup.zoom_y,frozen_system,true);
        SelectObject(dc,old_font);
        return;
    }
    const auto& apps=chart_apps(popup.kind,popup.held);
    if(popup.auto_scale && popup.axis_dirty) {
        popup.target_auto_zoom=auto_vertical_zoom(apps,from,to);
        if(!popup.axis_initialized) {
            popup.cached_auto_zoom=popup.target_auto_zoom;
            popup.axis_initialized=true;
        }
        popup.axis_dirty=false;
        SetTimer(hwnd_,animate_message,33,nullptr);
    }
    const double vertical_zoom=chart_effective_zoom(popup.auto_scale,
        popup.cached_auto_zoom,popup.zoom_y);
    draw_graph(dc,graph,apps,from,to,L"",popup.kind==0 && !popup.held.paused,vertical_zoom,true);
    // Hover uses the very same series and plotting scale as the detached graph.
    const int overscan=popup.kind==0 && !popup.held.paused && to>=live_view_end()-1000?live_tail_guard_px:0;
    if(const auto hit=auto_score_hit(apps,graph,popup.cursor,from,to,vertical_zoom,overscan)) {
        const int bubble_width=std::min(420,std::max(240,static_cast<int>(client.right)-36));
        const int bubble_height=static_cast<int>(std::lround(bubble_width*198.0/420.0));
        const int left=std::clamp(static_cast<int>(popup.cursor.x)+16,12,
            std::max(12,static_cast<int>(client.right)-bubble_width-12));
        const int title_bottom=layout.title_top+24;
        const int top=std::clamp(static_cast<int>(popup.cursor.y)+16,title_bottom,
            std::max(title_bottom,static_cast<int>(client.bottom)-bubble_height-12));
        const RECT bubble{left,top,left+bubble_width,top+bubble_height};
        draw_metric_tooltip(dc,bubble,apps[hit->app],hit->score,hit->sample_at);
    }
    SelectObject(dc,old_font);
}
LRESULT CALLBACK StatisticsPage::popup_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* popup=reinterpret_cast<PopupState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        const auto* created=reinterpret_cast<CREATESTRUCTW*>(lp);
        popup=static_cast<PopupState*>(created->lpCreateParams);
        popup->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(popup));
        return TRUE;
    }
    if(!popup || !popup->owner) return DefWindowProcW(window,msg,wp,lp);
    auto* page=popup->owner;
    switch(msg) {
    case WM_CREATE: {
        popup->auto_toggle=CreateWindowExW(0,L"BUTTON",L"Автомасштаб",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->auto_toggle) return -1;
        SendMessageW(popup->auto_toggle,BM_SETCHECK,popup->auto_scale?BST_CHECKED:BST_UNCHECKED,0);
        popup->fill_toggle=CreateWindowExW(0,L"BUTTON",L"Заливка графика",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,
            window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id+1)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->fill_toggle)return -1;
        SendMessageW(popup->fill_toggle,BM_SETCHECK,page->fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
        popup->pause_toggle=CreateWindowExW(0,L"BUTTON",L"Пауза",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id+2)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->pause_toggle)return -1;
        layout_detached_controls(window,{popup->auto_toggle,popup->fill_toggle,popup->pause_toggle});
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==popup_auto_id+2 && HIWORD(wp)==BN_CLICKED) {
            page->toggle_chart_pause(popup->kind,popup->held,popup->pause_toggle);
            popup->axis_dirty=true;
            InvalidateRect(window,nullptr,FALSE);return 0;
        }
        if(LOWORD(wp)==popup_auto_id+1 && HIWORD(wp)==BN_CLICKED) {
            page->set_chart_fill(SendMessageW(popup->fill_toggle,BM_GETCHECK,0,0)==BST_CHECKED);return 0;
        }
        if(LOWORD(wp)==popup_auto_id && HIWORD(wp)==BN_CLICKED) {
            page->set_popup_autoscale(*popup,SendMessageW(popup->auto_toggle,BM_GETCHECK,0,0)==BST_CHECKED);
            return 0;
        }
        break;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};const HDC dc=BeginPaint(window,&ps);
        RECT r{};GetClientRect(window,&r);
        if(r.right>0 && r.bottom>0) {
            if(const HDC buffer=popup->paint_buffer.prepare(dc,r.right,r.bottom)) {
                page->paint_popup(*popup,buffer);
                BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
            } else page->paint_popup(*popup,dc);
        }
        EndPaint(window,&ps);return 0;
    }
    case WM_MOUSEWHEEL: {
        const double factor=GET_WHEEL_DELTA_WPARAM(wp)>0?1.25:0.8;
        if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL) {
            if(popup->auto_scale) {
                const auto [from,to]=page->popup_range(*popup);
                if(popup->kind<2) {
                    const auto& apps=page->chart_apps(popup->kind,popup->held);
                    popup->zoom_y=auto_vertical_zoom(apps,from,to);
                } else popup->zoom_y=1.0;
                page->set_popup_autoscale(*popup,false);
            }
            popup->zoom_y=std::clamp(popup->zoom_y*factor,1.0,100.0);
        } else {
            const auto plot=detached_plot(detached_layout(window));
            POINT cursor{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            ScreenToClient(window,&cursor);
            const double fraction=std::clamp(static_cast<double>(cursor.x-plot.left)
                /static_cast<double>(std::max<LONG>(1,plot.right-plot.left)),0.0,1.0);
            const auto base=page->chart_range(popup->kind,popup->held);
            const double previous_limit=chart_popup_pan_limit(base,popup->zoom_x);
            popup->pan_ms=std::clamp(popup->pan_ms,-previous_limit,previous_limit);
            const auto [before_from,before_to]=chart_popup_window(base,popup->zoom_x,popup->pan_ms);
            const double at=static_cast<double>(before_from)
                +fraction*static_cast<double>(before_to-before_from);
            popup->zoom_x=std::clamp(popup->zoom_x*factor,1.0,128.0);
            const double limit=chart_popup_pan_limit(base,popup->zoom_x);
            popup->pan_ms=std::clamp(popup->pan_ms,-limit,limit);
            const auto [after_from,after_to]=chart_popup_window(base,popup->zoom_x,popup->pan_ms);
            popup->pan_ms=std::clamp(popup->pan_ms+at-(static_cast<double>(after_from)
                +fraction*static_cast<double>(after_to-after_from)),-limit,limit);
        }
        popup->axis_dirty=true;
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_LBUTTONDBLCLK: {
        popup->zoom_x=1;popup->zoom_y=1;popup->pan_ms=0;
        page->set_popup_autoscale(*popup,true);return 0;
    }
    case WM_LBUTTONDOWN:
        popup->dragging=true;popup->last={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        SetCapture(window);return 0;
    case WM_LBUTTONUP:
        popup->dragging=false;
        if(GetCapture()==window) ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:popup->dragging=false;return 0;
    case WM_MOUSEMOVE: {
        const POINT current{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(!popup->dragging && popup->cursor.x==current.x && popup->cursor.y==current.y)
            return 0; // Duplicate mouse messages must not redraw the whole graph.
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
        TrackMouseEvent(&track);
        popup->cursor=current;
        if(popup->kind>=2) {
            const auto chart=detached_plot(detached_layout(window));
            const auto [from,to]=page->popup_range(*popup);
            page->hover_system_charts(popup->kind,current,chart,from,to,popup->held);
        }
        if(popup->dragging) {
            const auto base=page->chart_range(popup->kind,popup->held);
            const double half_range=chart_popup_pan_limit(base,popup->zoom_x);
            popup->pan_ms=std::clamp(popup->pan_ms,-half_range,half_range);
            const auto [from,to]=chart_popup_window(base,popup->zoom_x,popup->pan_ms);
            const auto plot=detached_plot(detached_layout(window));
            const auto width=std::max<LONG>(1,plot.right-plot.left);
            popup->pan_ms-=static_cast<double>(current.x-popup->last.x)
                *static_cast<double>(to-from)/static_cast<double>(width);
            popup->pan_ms=std::clamp(popup->pan_ms,-half_range,half_range);
            popup->last=current;
            popup->axis_dirty=true;
        }
        // System popouts use the separate tooltip, so hovering must not
        // repaint a full chart at mouse-event frequency.
        if(popup->kind<2 || popup->dragging)InvalidateRect(window,nullptr,FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        popup->cursor={-1,-1};
        if(popup->kind>=2) {
            page->system_tip_active_=false;
            if(page->tooltip_)ShowWindow(page->tooltip_,SW_HIDE);
        }
        if(popup->kind<2)InvalidateRect(window,nullptr,FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* bounds=reinterpret_cast<MINMAXINFO*>(lp);
        const bool paired=popup->kind>=2 && chart_system_panels(popup->kind).secondary.has_value();
        bounds->ptMinTrackSize={paired?640:460,paired?640:340};
        return 0;
    }
    case WM_SIZE: {
        if(wp==SIZE_MINIMIZED) {popup->paint_buffer.clear();return 0;}
        if(popup->pause_toggle)layout_detached_controls(window,
            {popup->auto_toggle,popup->fill_toggle,popup->pause_toggle});
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:
        if(GetCapture()==window) ReleaseCapture();
        popup->paint_buffer.clear();
        popup->auto_toggle=nullptr;
        popup->fill_toggle=nullptr;
        popup->pause_toggle=nullptr;
        popup->held=FrozenView{};
        popup->cached_auto_zoom=1.0;popup->target_auto_zoom=1.0;
        popup->axis_initialized=false;popup->axis_dirty=true;
        popup->zoom_x=1.0;popup->zoom_y=1.0;popup->pan_ms=0.0;
        page->system_tip_active_=false;
        if(page->tooltip_)ShowWindow(page->tooltip_,SW_HIDE);
        popup->window=nullptr;popup->dragging=false;return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}
