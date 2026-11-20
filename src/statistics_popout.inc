// Reuse the StatisticsPage model and colour map: detached windows never collect data.
void StatisticsPage::popup_range(const PopupState& popup,std::int64_t& from,std::int64_t& to) const {
    const auto end=popup.paused?popup.frozen_to:(popup.kind>=2?system_view_end():
        popup.kind==0?live_view_end():window_to_);
    const auto begin=popup.paused?popup.frozen_from:(popup.kind>=2?end-std::array<std::int64_t,3>{{60000,300000,900000}}[system_range_]:
        popup.kind==0?end-60000:window_from_);
    const double total=static_cast<double>(std::max<std::int64_t>(1000,end-begin));
    const double span=std::max(1000.0,total/std::clamp(popup.zoom_x,1.0,128.0));
    const double room=std::max(0.0,(total-span)/2.0);
    const double center=(static_cast<double>(begin)+static_cast<double>(end))/2.0
        +std::clamp(popup.pan_ms,-room,room);
    from=static_cast<std::int64_t>(center-span/2.0);
    to=static_cast<std::int64_t>(center+span/2.0);
    if(to<=from) to=from+1000;
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
    if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error("Register detached statistics graph");
    popup.owner=this;popup.kind=kind;
    popup.zoom_x=1.0;popup.zoom_y=1.0;popup.pan_ms=0;
    constexpr std::array<const wchar_t*,6> auto_keys{{L"stats_popup_auto_live",L"stats_popup_auto_history",
        L"stats_popup_auto_cpu",L"stats_popup_auto_ram",L"stats_popup_auto_network",L"stats_popup_auto_gpu"}};
    popup.auto_scale=GetPrivateProfileIntW(L"overlay",auto_keys[kind],1,settings_.c_str())!=0;
    popup.dragging=false;popup.cursor={-1,-1};
    const std::array<const wchar_t*,6> titles{{L"FluxTop — последняя минута (AutoScore)",
        L"FluxTop — история (AutoScore)",L"FluxTop — ЦП",L"FluxTop — память",
        L"FluxTop — сеть",L"FluxTop — GPU"}};
    const wchar_t* title=titles[kind];
    popup.window=CreateWindowExW(0,window_class,title,WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,1050,590,nullptr,nullptr,GetModuleHandleW(nullptr),&popup);
    if(!popup.window) throw windows_error("Create detached statistics graph");
    ShowWindow(popup.window,SW_SHOW);
    UpdateWindow(popup.window);
    // The historical popout keeps refreshing even when the Settings tab is active.
    if(kind==1) request();
}
void StatisticsPage::paint_popup(PopupState& popup,HDC dc) {
    RECT client{};GetClientRect(popup.window,&client);
    if(client.right<180 || client.bottom<180) return;
    // Native background and labels; the graph retains its hand-drawn look.
    FillRect(dc,&client,GetSysColorBrush(COLOR_BTNFACE));
    if(!popup_font_) popup_font_=CreateFontW(-17,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const auto old_font=SelectObject(dc,popup_font_?popup_font_:GetStockObject(DEFAULT_GUI_FONT));
    const std::array<const wchar_t*,6> names{{L"AutoScore · последняя минута",L"AutoScore · история",
        L"ЦП",L"Память",L"Сеть",L"GPU"}};
    label(dc,names[popup.kind],22,55);
    // Controls are native Win32; navigation instructions are intentionally absent.
    std::int64_t from{},to{};popup_range(popup,from,to);
    const RECT graph{68,114,client.right-30,client.bottom-60};
    const auto* frozen_system=popup.paused?&popup.frozen_system:nullptr;
    if(popup.kind>=2) {
        if(popup.kind==5) {
            const LONG middle=114+(client.bottom-174)/2;
            const RECT gpu{68,114,client.right-30,middle-23};
            const RECT vram{68,middle+26,client.right-30,client.bottom-60};
            label(dc,L"Загрузка GPU",170,90,RGB(131,54,156));
            label(dc,L"Использование VRAM",170,middle+4,RGB(194,89,178));
            draw_system_graph(dc,gpu,3,from,to,popup.auto_scale,popup.zoom_y,popup.cursor,frozen_system,true);
            draw_system_graph(dc,vram,4,from,to,popup.auto_scale,popup.zoom_y,popup.cursor,frozen_system,true);
        } else draw_system_graph(dc,graph,popup.kind-2,from,to,popup.auto_scale,popup.zoom_y,popup.cursor,frozen_system,true);
        SelectObject(dc,old_font);
        return;
    }
    const auto& apps=popup.paused?popup.frozen_apps:(popup.kind==0?live_apps_:history_.history);
    if(popup.auto_scale && popup.axis_dirty) {
        popup.target_auto_zoom=auto_vertical_zoom(apps,from,to);
        if(!popup.axis_initialized) {
            popup.cached_auto_zoom=popup.target_auto_zoom;
            popup.axis_initialized=true;
        }
        popup.axis_dirty=false;
        SetTimer(hwnd_,animate_message,33,nullptr);
    }
    const double vertical_zoom=popup.auto_scale?popup.cached_auto_zoom:popup.zoom_y;
    draw_graph(dc,graph,apps,from,to,L"",popup.kind==0 && !popup.paused,vertical_zoom,true);
    // Hover uses the very same series and plotting scale as the detached graph.
    if(popup.cursor.x>=graph.left && popup.cursor.x<=graph.right
        && popup.cursor.y>=graph.top && popup.cursor.y<=graph.bottom && to>from) {
        double best=144.0;std::size_t selected=0;double score=0;std::int64_t sample_at=0;bool found=false;
        const double width=static_cast<double>(graph.right-graph.left);
        const double height=static_cast<double>(graph.bottom-graph.top);
        for(std::size_t i=0;i<apps.size();++i) {
            const auto& pts=apps[i].points;
            for(std::size_t j=0;j<pts.size();++j) {
                const auto xpos=graph.left+static_cast<double>(pts[j].first-from)*width/static_cast<double>(to-from);
                if(xpos>=graph.left-20 && xpos<=graph.right+20) {
                    const double ypos=graph.bottom-std::clamp(pts[j].second*vertical_zoom,0.0,100.0)*height/100.0;
                    const double distance=(xpos-popup.cursor.x)*(xpos-popup.cursor.x)
                        +(ypos-popup.cursor.y)*(ypos-popup.cursor.y);
                    if(distance<best) {best=distance;selected=i;score=pts[j].second;sample_at=pts[j].first;found=true;}
                }
                // A line can cross the viewport with both stored points outside it.
                if(j==0) continue;
                const auto old_x=graph.left+static_cast<double>(pts[j-1].first-from)*width/static_cast<double>(to-from);
                if(xpos<=old_x || popup.cursor.x<old_x || popup.cursor.x>xpos) continue;
                const double fraction=(popup.cursor.x-old_x)/(xpos-old_x);
                const double value=pts[j-1].second+(pts[j].second-pts[j-1].second)*fraction;
                const double line_y=graph.bottom-std::clamp(value*vertical_zoom,0.0,100.0)*height/100.0;
                const double dx=line_y-popup.cursor.y;
                if(dx*dx<best) {best=dx*dx;selected=i;score=value;sample_at=pts[j-1].first+static_cast<std::int64_t>(static_cast<double>(pts[j].first-pts[j-1].first)*fraction);found=true;}
            }
        }
        if(found && selected<apps.size()) {
            const auto& app=apps[selected];
            const int bubble_width=std::min(420,std::max(240,static_cast<int>(client.right)-36));
            const int bubble_height=static_cast<int>(std::lround(bubble_width*198.0/420.0));
            const int left=std::clamp(static_cast<int>(popup.cursor.x)+16,12,
                std::max(12,static_cast<int>(client.right)-bubble_width-12));
            const int top=std::clamp(static_cast<int>(popup.cursor.y)+16,76,
                std::max(76,static_cast<int>(client.bottom)-bubble_height-12));
            const RECT bubble{left,top,left+bubble_width,top+bubble_height};
            draw_metric_tooltip(dc,bubble,app,score,sample_at);
        }
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
            22,8,180,28,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->auto_toggle) return -1;
        SendMessageW(popup->auto_toggle,BM_SETCHECK,popup->auto_scale?BST_CHECKED:BST_UNCHECKED,0);
        popup->fill_toggle=CreateWindowExW(0,L"BUTTON",L"Заливка графика",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,220,8,190,28,
            window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id+1)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->fill_toggle)return -1;
        SendMessageW(popup->fill_toggle,BM_SETCHECK,page->fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
        popup->pause_toggle=CreateWindowExW(0,L"BUTTON",L"Пауза",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,445,8,126,28,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(popup_auto_id+2)),
            GetModuleHandleW(nullptr),nullptr);
        if(!popup->pause_toggle)return -1;
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==popup_auto_id+2 && HIWORD(wp)==BN_CLICKED) {
            if(popup->paused) {
                popup->paused=false;
                popup->frozen_system.clear();popup->frozen_apps.clear();
                popup->frozen_from=0;popup->frozen_to=0;
            } else {
                const auto latest=popup->kind>=2?page->system_view_end():
                    (popup->kind==0?page->live_view_end():page->window_to_);
                popup->frozen_to=latest;
                popup->frozen_from=popup->kind>=2?
                    latest-std::array<std::int64_t,3>{{60000,300000,900000}}[page->system_range_]:
                    (popup->kind==0?latest-60000:page->window_from_);
                if(popup->kind>=2)popup->frozen_system=page->system_points_;
                else popup->frozen_apps=popup->kind==0?page->live_apps_:page->history_.history;
                popup->paused=true;
            }
            SetWindowTextW(popup->pause_toggle,popup->paused?L"Продолжить":L"Пауза");
            popup->axis_dirty=true;
            InvalidateRect(window,nullptr,FALSE);return 0;
        }
        if(LOWORD(wp)==popup_auto_id+1 && HIWORD(wp)==BN_CLICKED) {
            page->fill_enabled_=SendMessageW(popup->fill_toggle,BM_GETCHECK,0,0)==BST_CHECKED;
            WritePrivateProfileStringW(L"overlay",L"stats_chart_fill",
                page->fill_enabled_?L"1":L"0",page->settings_.c_str());
            if(page->fill_toggle_)SendMessageW(page->fill_toggle_,BM_SETCHECK,
                page->fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
            for(auto& other:page->popups_)if(other.window) {
                if(other.fill_toggle)SendMessageW(other.fill_toggle,BM_SETCHECK,
                    page->fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
                InvalidateRect(other.window,nullptr,FALSE);
            }
            InvalidateRect(page->hwnd_,nullptr,FALSE);return 0;
        }
        if(LOWORD(wp)==popup_auto_id && HIWORD(wp)==BN_CLICKED) {
            popup->auto_scale=SendMessageW(popup->auto_toggle,BM_GETCHECK,0,0)==BST_CHECKED;
            popup->axis_dirty=true;
            constexpr std::array<const wchar_t*,6> keys{{L"stats_popup_auto_live",L"stats_popup_auto_history",
                L"stats_popup_auto_cpu",L"stats_popup_auto_ram",L"stats_popup_auto_network",L"stats_popup_auto_gpu"}};
            WritePrivateProfileStringW(L"overlay",keys[popup->kind],
                popup->auto_scale?L"1":L"0",page->settings_.c_str());
            InvalidateRect(window,nullptr,FALSE);
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
                std::int64_t from{},to{};page->popup_range(*popup,from,to);
                if(popup->kind<2) {
                    const auto& apps=popup->paused?popup->frozen_apps:
                    (popup->kind==0?page->live_apps_:page->history_.history);
                    popup->zoom_y=auto_vertical_zoom(apps,from,to);
                } else popup->zoom_y=1.0;
                popup->auto_scale=false;
                SendMessageW(popup->auto_toggle,BM_SETCHECK,BST_UNCHECKED,0);
                constexpr std::array<const wchar_t*,6> keys{{L"stats_popup_auto_live",L"stats_popup_auto_history",
                    L"stats_popup_auto_cpu",L"stats_popup_auto_ram",L"stats_popup_auto_network",L"stats_popup_auto_gpu"}};
                WritePrivateProfileStringW(L"overlay",keys[popup->kind],L"0",page->settings_.c_str());
            }
            popup->zoom_y=std::clamp(popup->zoom_y*factor,1.0,100.0);
        } else {
            RECT r{};GetClientRect(window,&r);
            POINT cursor{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            ScreenToClient(window,&cursor);
            const double fraction=std::clamp((static_cast<double>(cursor.x)-68.0)
                /static_cast<double>(std::max<LONG>(1,r.right-98)),0.0,1.0);
            std::int64_t before_from{},before_to{};
            page->popup_range(*popup,before_from,before_to);
            const double at=static_cast<double>(before_from)
                +fraction*static_cast<double>(before_to-before_from);
            popup->zoom_x=std::clamp(popup->zoom_x*factor,1.0,128.0);
            std::int64_t after_from{},after_to{};
            page->popup_range(*popup,after_from,after_to);
            popup->pan_ms+=at-(static_cast<double>(after_from)
                +fraction*static_cast<double>(after_to-after_from));
        }
        popup->axis_dirty=true;
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_LBUTTONDBLCLK: {
        popup->zoom_x=1;popup->zoom_y=1;popup->pan_ms=0;
        popup->auto_scale=true;
        popup->axis_dirty=true;
        SendMessageW(popup->auto_toggle,BM_SETCHECK,BST_CHECKED,0);
        constexpr std::array<const wchar_t*,6> keys{{L"stats_popup_auto_live",L"stats_popup_auto_history",
            L"stats_popup_auto_cpu",L"stats_popup_auto_ram",L"stats_popup_auto_network",L"stats_popup_auto_gpu"}};
        WritePrivateProfileStringW(L"overlay",keys[popup->kind],L"1",page->settings_.c_str());
        InvalidateRect(window,nullptr,FALSE);return 0;
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
            RECT bounds{};GetClientRect(window,&bounds);
            const RECT chart{68,114,bounds.right-30,bounds.bottom-60};
            std::int64_t from{},to{};page->popup_range(*popup,from,to);
            if(popup->kind==5) {
                const LONG middle=114+(bounds.bottom-174)/2;
                const RECT gpu{68,114,bounds.right-30,middle-23};
                const RECT vram{68,middle+26,bounds.right-30,bounds.bottom-60};
                if(current.y>=vram.top && current.y<=vram.bottom)
                    page->show_system_hover(4,current,vram,from,to,
                        popup->paused?&popup->frozen_system:nullptr);
                else page->show_system_hover(3,current,gpu,from,to,
                        popup->paused?&popup->frozen_system:nullptr);
            } else page->show_system_hover(popup->kind-2,current,chart,from,to,
                popup->paused?&popup->frozen_system:nullptr);
        }
        if(popup->dragging) {
            std::int64_t from{},to{};page->popup_range(*popup,from,to);
            RECT r{};GetClientRect(window,&r);
            const auto width=std::max<LONG>(1,r.right-98);
            popup->pan_ms-=static_cast<double>(current.x-popup->last.x)
                *static_cast<double>(to-from)/static_cast<double>(width);
            const auto full=popup->kind>=2?
                static_cast<double>(std::array<std::int64_t,3>{{60000,300000,900000}}[page->system_range_]):
                popup->kind==0?60000.0:
                static_cast<double>(std::max<std::int64_t>(1000,page->window_to_-page->window_from_));
            const double half_range=std::max(0.0,(full-full/popup->zoom_x)/2.0);
            popup->pan_ms=std::clamp(popup->pan_ms,-half_range,half_range);
            popup->last=current;
            popup->axis_dirty=true;
        }
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_MOUSELEAVE:
        popup->cursor={-1,-1};
        if(popup->kind>=2) {
            page->system_tip_active_=false;
            if(page->tooltip_)ShowWindow(page->tooltip_,SW_HIDE);
        }
        InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_GETMINMAXINFO: {
        auto* bounds=reinterpret_cast<MINMAXINFO*>(lp);
        bounds->ptMinTrackSize={460,340};
        return 0;
    }
    case WM_SIZE: {
        if(wp==SIZE_MINIMIZED) {popup->paint_buffer.clear();return 0;}
        RECT r{};GetClientRect(window,&r);
        if(popup->auto_toggle)MoveWindow(popup->auto_toggle,
            std::max<LONG>(15,r.right-410),8,180,28,TRUE);
        if(popup->fill_toggle)MoveWindow(popup->fill_toggle,
            std::max<LONG>(205,r.right-210),8,195,28,TRUE);
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:
        if(GetCapture()==window) ReleaseCapture();
        popup->paint_buffer.clear();
        popup->auto_toggle=nullptr;
        popup->fill_toggle=nullptr;
        popup->pause_toggle=nullptr;
        popup->paused=false;
        popup->frozen_system.clear();popup->frozen_apps.clear();
        page->system_tip_active_=false;
        if(page->tooltip_)ShowWindow(page->tooltip_,SW_HIDE);
        popup->window=nullptr;popup->dragging=false;return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}
