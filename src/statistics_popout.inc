// Reuse the StatisticsPage model and colour map: detached windows never collect data.
void StatisticsPage::popup_range(const PopupState& popup,std::int64_t& from,std::int64_t& to) const {
    const auto end=popup.kind==0?now_ms():window_to_;
    const auto begin=popup.kind==0?end-60000:window_from_;
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
    if(kind>1) return;
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
    popup.dragging=false;popup.cursor={-1,-1};
    const wchar_t* title=kind==0?L"FluxTop — последняя минута (AutoScore)"
        :L"FluxTop — история (AutoScore)";
    const HWND owner=GetAncestor(hwnd_,GA_ROOT);
    popup.window=CreateWindowExW(0,window_class,title,WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,1050,590,owner,nullptr,GetModuleHandleW(nullptr),&popup);
    if(!popup.window) throw windows_error("Create detached statistics graph");
    ShowWindow(popup.window,SW_SHOW);
    UpdateWindow(popup.window);
    // The historical popout keeps refreshing even when the Settings tab is active.
    if(kind==1) request();
}
void StatisticsPage::paint_popup(PopupState& popup,HDC dc) {
    RECT client{};GetClientRect(popup.window,&client);
    if(client.right<180 || client.bottom<180) return;
    HBRUSH surface=CreateSolidBrush(RGB(245,246,247));
    FillRect(dc,&client,surface);DeleteObject(surface);
    HFONT font=CreateFontW(-17,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const auto old_font=SelectObject(dc,font);
    label(dc,popup.kind==0?L"AutoScore · последняя минута":L"AutoScore · история",22,18);
    HFONT help_font=CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SelectObject(dc,help_font);
    label(dc,L"Колесо: время  ·  Ctrl+колесо: баллы  ·  ЛКМ: двигать  ·  двойной клик: сброс",22,46,RGB(90,97,108));
    std::int64_t from{},to{};popup_range(popup,from,to);
    const RECT graph{68,112,client.right-30,client.bottom-61};
    const auto& apps=popup.kind==0?live_apps_:history_.history;
    draw_graph(dc,graph,apps,from,to,L"",popup.kind==0,popup.zoom_y,true);
    wchar_t zoom[128]{};
    swprintf_s(zoom,L"Время: %.1fx    Баллы: %.1fx    Серий: %u",
        popup.zoom_x,popup.zoom_y,static_cast<unsigned>(apps.size()));
    label(dc,zoom,70,client.bottom-29,RGB(78,85,95));
    // Hover uses the very same series and plotting scale as the detached graph.
    if(popup.cursor.x>=graph.left && popup.cursor.x<=graph.right
        && popup.cursor.y>=graph.top && popup.cursor.y<=graph.bottom && to>from) {
        double best=144.0;std::size_t selected=0;double score=0;bool found=false;
        const double width=static_cast<double>(graph.right-graph.left);
        const double height=static_cast<double>(graph.bottom-graph.top);
        for(std::size_t i=0;i<apps.size();++i) {
            const auto& pts=apps[i].points;
            for(std::size_t j=0;j<pts.size();++j) {
                const auto xpos=graph.left+static_cast<double>(pts[j].first-from)*width/static_cast<double>(to-from);
                if(xpos<graph.left-20 || xpos>graph.right+20) continue;
                const double ypos=graph.bottom-std::clamp(pts[j].second*popup.zoom_y,0.0,100.0)*height/100.0;
                const double distance=(xpos-popup.cursor.x)*(xpos-popup.cursor.x)
                    +(ypos-popup.cursor.y)*(ypos-popup.cursor.y);
                if(distance<best) {best=distance;selected=i;score=pts[j].second;found=true;}
                if(j==0) continue;
                const auto old_x=graph.left+static_cast<double>(pts[j-1].first-from)*width/static_cast<double>(to-from);
                if(xpos<=old_x || popup.cursor.x<old_x || popup.cursor.x>xpos) continue;
                const double fraction=(popup.cursor.x-old_x)/(xpos-old_x);
                const double value=pts[j-1].second+(pts[j].second-pts[j-1].second)*fraction;
                const double line_y=graph.bottom-std::clamp(value*popup.zoom_y,0.0,100.0)*height/100.0;
                const double dx=line_y-popup.cursor.y;
                if(dx*dx<best) {best=dx*dx;selected=i;score=value;found=true;}
            }
        }
        if(found && selected<apps.size()) {
            const auto& app=apps[selected];
            const int left=std::clamp(static_cast<int>(popup.cursor.x)+14,12,std::max(12,static_cast<int>(client.right)-260));
            const int top=std::clamp(static_cast<int>(popup.cursor.y)+14,80,std::max(80,static_cast<int>(client.bottom)-80));
            const RECT bubble{left,top,left+245,top+64};
            HBRUSH bg=CreateSolidBrush(RGB(255,255,249));FillRect(dc,&bubble,bg);DeleteObject(bg);
            HPEN border=CreatePen(PS_SOLID,1,RGB(190,198,205));
            const auto old_pen=SelectObject(dc,border);
            const auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,bubble.left,bubble.top,bubble.right,bubble.bottom);
            SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(border);
            if(auto icon=application_icon(app.path)) DrawIconEx(dc,left+9,top+19,icon,24,24,0,nullptr,DI_NORMAL);
            RECT name{left+39,top+7,left+235,top+33};
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(39,43,48));
            DrawTextW(dc,app.name.c_str(),-1,&name,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            wchar_t value[70]{};swprintf_s(value,L"AutoScore: %.1f / 100",score);
            SetTextColor(dc,series_color(app,selected));
            TextOutW(dc,left+39,top+36,value,static_cast<int>(wcslen(value)));
        }
    }
    SelectObject(dc,old_font);DeleteObject(help_font);DeleteObject(font);
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
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};const HDC dc=BeginPaint(window,&ps);
        RECT r{};GetClientRect(window,&r);
        HDC buffer=CreateCompatibleDC(dc);
        HBITMAP bitmap=buffer?CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom)):nullptr;
        if(bitmap) {
            const auto old=SelectObject(buffer,bitmap);
            page->paint_popup(*popup,buffer);
            BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,old);
        } else page->paint_popup(*popup,dc);
        if(bitmap) DeleteObject(bitmap);
        if(buffer) DeleteDC(buffer);
        EndPaint(window,&ps);return 0;
    }
    case WM_MOUSEWHEEL: {
        const double factor=GET_WHEEL_DELTA_WPARAM(wp)>0?1.25:0.8;
        if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL)
            popup->zoom_y=std::clamp(popup->zoom_y*factor,1.0,16.0);
        else popup->zoom_x=std::clamp(popup->zoom_x*factor,1.0,128.0);
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_LBUTTONDBLCLK:
        popup->zoom_x=1;popup->zoom_y=1;popup->pan_ms=0;
        InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_LBUTTONDOWN:
        popup->dragging=true;popup->last={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        SetCapture(window);return 0;
    case WM_LBUTTONUP:
        popup->dragging=false;
        if(GetCapture()==window) ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:popup->dragging=false;return 0;
    case WM_MOUSEMOVE: {
        const POINT current{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        popup->cursor=current;
        if(popup->dragging) {
            std::int64_t from{},to{};page->popup_range(*popup,from,to);
            RECT r{};GetClientRect(window,&r);
            const auto width=std::max<LONG>(1,r.right-98);
            popup->pan_ms-=static_cast<double>(current.x-popup->last.x)
                *static_cast<double>(to-from)/static_cast<double>(width);
            popup->last=current;
        }
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_SIZE:InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:
        if(GetCapture()==window) ReleaseCapture();
        popup->window=nullptr;popup->dragging=false;return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}