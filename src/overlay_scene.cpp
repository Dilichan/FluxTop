#include "fluxtop/overlay_scene.hpp"
#include "fluxtop/app_icons.hpp"
#include "fluxtop/windows.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <array>
#include <cmath>

namespace fluxtop {
namespace {
struct Display {
    HMONITOR handle;
    MONITORINFOEXW info;
};

std::vector<Display> displays() {
    std::vector<Display> result;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM param)->BOOL {
        Display info{monitor,{sizeof(MONITORINFOEXW)}};
        if(GetMonitorInfoW(monitor,&info.info))
            reinterpret_cast<std::vector<Display>*>(param)->push_back(info);
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
    return result;
}

INT_PTR modal(HWND owner,DLGPROC proc,LPARAM state,short width,short height) {
    struct alignas(DWORD) Template { DLGTEMPLATE header; WORD end[3]; };
    const Template layout{{WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER,0,0,0,0,width,height},{}};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.header,owner,proc,state);
}

HWND control(HWND parent,const wchar_t* type,const wchar_t* text,DWORD style,int id,
    int x,int y,int width,int height) {
    const auto window=CreateWindowExW(0,type,text,WS_VISIBLE|WS_CHILD|style,x,y,width,height,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(window)SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    return window;
}

struct Scene {
    std::vector<Display> screens;
    std::size_t screen{};
    OverlayScenePosition position;
    SIZE size{};
    int margin{};
    POINT grab{};
    bool dragging{};

    RECT screen_area(HWND hwnd) const {
        RECT available{};GetClientRect(hwnd,&available);
        InflateRect(&available,-16,-12);
        available.top+=64;available.bottom-=43;
        const auto& work=screens[screen].info.rcWork;
        const auto factor=std::min(static_cast<double>(available.right-available.left)/(work.right-work.left),
            static_cast<double>(available.bottom-available.top)/(work.bottom-work.top));
        const auto width=static_cast<LONG>(std::lround(factor*(work.right-work.left)));
        const auto height=static_cast<LONG>(std::lround(factor*(work.bottom-work.top)));
        const auto x=(available.left+available.right-width)/2;
        const auto y=(available.top+available.bottom-height)/2;
        return {x,y,x+width,y+height};
    }
    RECT overlay_rect(const RECT& canvas) const {
        const auto& work=screens[screen].info.rcWork;
        const int width=work.right-work.left,height=work.bottom-work.top;
        const int w=std::clamp(static_cast<int>(size.cx),1,std::max(1,width-margin*2));
        const int h=std::clamp(static_cast<int>(size.cy),1,std::max(1,height-margin*2));
        const auto x=scene_axis_offset(width,w,margin,position.x);
        const auto y=scene_axis_offset(height,h,margin,position.y);
        return {canvas.left+MulDiv(x,canvas.right-canvas.left,width),
            canvas.top+MulDiv(y,canvas.bottom-canvas.top,height),
            canvas.left+MulDiv(x+w,canvas.right-canvas.left,width),
            canvas.top+MulDiv(y+h,canvas.bottom-canvas.top,height)};
    }
    void move(HWND hwnd,POINT cursor) {
        const auto canvas=screen_area(hwnd);
        const auto& work=screens[screen].info.rcWork;
        const int width=work.right-work.left,height=work.bottom-work.top;
        const int w=std::clamp(static_cast<int>(size.cx),1,std::max(1,width-margin*2));
        const int h=std::clamp(static_cast<int>(size.cy),1,std::max(1,height-margin*2));
        position.x=scene_axis_position(width,w,margin,MulDiv(cursor.x-grab.x-canvas.left,width,canvas.right-canvas.left));
        position.y=scene_axis_position(height,h,margin,MulDiv(cursor.y-grab.y-canvas.top,height,canvas.bottom-canvas.top));
        InvalidateRect(hwnd,&canvas,FALSE);
    }
};
constexpr int monitor_control=120, blacklist_control=121;

INT_PTR CALLBACK scene_message(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_INITDIALOG) {
        auto& scene=*reinterpret_cast<Scene*>(lp);
        SetWindowLongPtrW(hwnd,DWLP_USER,lp);
        SetWindowTextW(hwnd,L"Сцена оверлея");
        RECT rect{};GetClientRect(hwnd,&rect);
        control(hwnd,L"STATIC",L"Перетащите оверлей на нужное место или щёлкните на сетке.",0,0,16,12,rect.right-32,20);
        const auto combo=control(hwnd,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,
            monitor_control,16,35,263,180);
        for(std::size_t i=0;i<scene.screens.size();++i) {
            const auto& work=scene.screens[i].info.rcWork;
            const auto text=L"Экран "+std::to_wstring(i+1)+L" · "+
                std::to_wstring(work.right-work.left)+L" × "+std::to_wstring(work.bottom-work.top);
            SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(combo,CB_SETCURSEL,scene.screen,0);
        control(hwnd,L"BUTTON",L"Готово",BS_DEFPUSHBUTTON|WS_TABSTOP,IDOK,rect.right-220,rect.bottom-41,94,29);
        control(hwnd,L"BUTTON",L"Отмена",BS_PUSHBUTTON|WS_TABSTOP,IDCANCEL,rect.right-112,rect.bottom-41,94,29);
        return TRUE;
    }
    auto* scene=reinterpret_cast<Scene*>(GetWindowLongPtrW(hwnd,DWLP_USER));
    if(!scene)return FALSE;
    switch(message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};const auto dc=BeginPaint(hwnd,&ps);
        const auto canvas=scene->screen_area(hwnd);
        FillRect(dc,&canvas,GetSysColorBrush(COLOR_WINDOW));
        const auto old=SelectObject(dc,GetStockObject(DC_PEN));
        SetDCPenColor(dc,RGB(216,219,225));
        for(int i=1;i<12;++i) {
            const int x=canvas.left+MulDiv(canvas.right-canvas.left,i,12);
            const int y=canvas.top+MulDiv(canvas.bottom-canvas.top,i,12);
            MoveToEx(dc,x,canvas.top,nullptr);LineTo(dc,x,canvas.bottom);
            MoveToEx(dc,canvas.left,y,nullptr);LineTo(dc,canvas.right,y);
        }
        SelectObject(dc,old);
        FrameRect(dc,&canvas,GetSysColorBrush(COLOR_BTNSHADOW));
        auto preview=scene->overlay_rect(canvas);
        FillRect(dc,&preview,GetSysColorBrush(COLOR_HIGHLIGHT));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));
        DrawTextW(dc,L"Оверлей",-1,&preview,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        EndPaint(hwnd,&ps);return TRUE;
    }
    case WM_LBUTTONDOWN: {
        const POINT mouse{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        const auto canvas=scene->screen_area(hwnd);
        if(!PtInRect(&canvas,mouse))return FALSE;
        const auto box=scene->overlay_rect(canvas);
        scene->grab=PtInRect(&box,mouse)?POINT{mouse.x-box.left,mouse.y-box.top}:
            POINT{(box.right-box.left)/2,(box.bottom-box.top)/2};
        scene->dragging=true;SetCapture(hwnd);scene->move(hwnd,mouse);return TRUE;
    }
    case WM_MOUSEMOVE:
        if(scene->dragging)scene->move(hwnd,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        return TRUE;
    case WM_LBUTTONUP:
        if(scene->dragging)ReleaseCapture();
        return TRUE;
    case WM_CAPTURECHANGED: scene->dragging=false;return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==monitor_control && HIWORD(wp)==CBN_SELCHANGE) {
            const auto index=SendDlgItemMessageW(hwnd,monitor_control,CB_GETCURSEL,0,0);
            if(index>=0 && static_cast<std::size_t>(index)<scene->screens.size()) {
                scene->screen=static_cast<std::size_t>(index);
                InvalidateRect(hwnd,nullptr,TRUE);
            }
            return TRUE;
        }
        if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)EndDialog(hwnd,LOWORD(wp));
        return TRUE;
    case WM_CLOSE: EndDialog(hwnd,IDCANCEL);return TRUE;
    }
    return FALSE;
}

struct Blacklist {
    const std::vector<OverlayApplication>& apps;
    AppIconCache& cache;
    HWND& dialog;
    const std::function<void(std::size_t,bool)>& changed;
    HWND list{};
    HIMAGELIST images{};
    int fallback{-1};
    int first{}, last{};
    std::vector<bool> has_icon;
    bool ready{};

    void set_icon(int row,int image) const {
        LVITEMW item{};item.mask=LVIF_IMAGE;item.iItem=row;item.iImage=image;
        ListView_SetItem(list,&item);
    }
    void refresh_icons() {
        if(!images)return;
        const int start=std::max(0,ListView_GetTopIndex(list));
        const int end=std::min(ListView_GetItemCount(list),start+ListView_GetCountPerPage(list)+1);
        if(start!=first || end!=last) {
            for(int i=first;i<last;++i)set_icon(i,-1);
            for(int i=ImageList_GetImageCount(images)-1;i>fallback;--i)ImageList_Remove(images,i);
            first=start;last=end;
            has_icon.assign(static_cast<std::size_t>(std::max(0,last-first)),false);
        }
        for(int i=first;i<last;++i) {
            const auto offset=static_cast<std::size_t>(i-first);
            if(has_icon[offset])continue;
            const auto& path=apps[static_cast<std::size_t>(i)].icon_path;
            const auto data=cache.get(path);
            if(!data) {set_icon(i,fallback);cache.request(path);continue;}
            if(const auto icon=native_app_icon(*data)) {
                const int index=ImageList_AddIcon(images,icon);
                DestroyIcon(icon);
                if(index>=0){set_icon(i,index);has_icon[offset]=true;}
            }
        }
    }
};

INT_PTR CALLBACK blacklist_message(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_INITDIALOG) {
        auto& state=*reinterpret_cast<Blacklist*>(lp);
        state.dialog=hwnd;
        SetWindowLongPtrW(hwnd,DWLP_USER,lp);
        SetWindowTextW(hwnd,L"Чёрный список оверлея");
        RECT rect{};GetClientRect(hwnd,&rect);
        control(hwnd,L"STATIC",state.apps.empty()?L"FluxTop ещё не обнаружил приложения с FPS.":
            L"Отметьте приложения, в которых нужно скрывать оверлей.",0,0,16,12,rect.right-32,24);
        const auto list=control(hwnd,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHAREIMAGELISTS|WS_TABSTOP|WS_BORDER,
            blacklist_control,16,43,rect.right-32,rect.bottom-98);
        state.list=list;
        const int icon_size=MulDiv(16,static_cast<int>(GetDpiForWindow(hwnd)),96);
        state.images=ImageList_Create(icon_size,icon_size,ILC_COLOR32|ILC_MASK,16,8);
        if(state.images) {
            if(const auto icon=LoadIconW(nullptr,IDI_APPLICATION))state.fallback=ImageList_AddIcon(state.images,icon);
            ListView_SetImageList(list,state.images,LVSIL_SMALL);
        }
        ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;
        column.pszText=const_cast<wchar_t*>(L"Приложение");column.cx=(rect.right-56)/2;
        ListView_InsertColumn(list,0,&column);
        column.pszText=const_cast<wchar_t*>(L"EXE");
        ListView_InsertColumn(list,1,&column);
        for(std::size_t i=0;i<state.apps.size();++i) {
            LVITEMW item{};item.mask=LVIF_TEXT|LVIF_IMAGE;item.iItem=static_cast<int>(i);item.iImage=-1;
            item.pszText=const_cast<wchar_t*>(state.apps[i].name.c_str());
            ListView_InsertItem(list,&item);
            ListView_SetItemText(list,item.iItem,1,const_cast<wchar_t*>(state.apps[i].executable.c_str()));
            ListView_SetCheckState(list,item.iItem,state.apps[i].blocked);
        }
        state.ready=true;
        control(hwnd,L"BUTTON",L"Готово",BS_DEFPUSHBUTTON|WS_TABSTOP,IDOK,rect.right-112,rect.bottom-42,94,29);
        PostMessageW(hwnd,blacklist_icons_ready,0,0);
        return TRUE;
    }
    if(message==WM_CLOSE || (message==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL))) {
        EndDialog(hwnd,IDOK);return TRUE;
    }
    auto* state=reinterpret_cast<Blacklist*>(GetWindowLongPtrW(hwnd,DWLP_USER));
    if(!state)return FALSE;
    if(message==blacklist_icons_ready) {
        state->refresh_icons();return TRUE;
    }
    if(message==WM_DESTROY) {
        if(state->images) {
            ListView_SetImageList(state->list,nullptr,LVSIL_SMALL);
            ImageList_Destroy(state->images);
        }
        state->dialog=nullptr;return TRUE;
    }
    if(message==WM_NOTIFY) {
        const auto& header=*reinterpret_cast<NMHDR*>(lp);
        if(!state->ready || header.idFrom!=blacklist_control)return FALSE;
        if(header.code==LVN_ENDSCROLL) {
            PostMessageW(hwnd,blacklist_icons_ready,0,0);return FALSE;
        }
        if(header.code!=LVN_ITEMCHANGED)
            return FALSE;
        const auto& item=*reinterpret_cast<NMLISTVIEW*>(lp);
        if(item.iItem>=0 && static_cast<std::size_t>(item.iItem)<state->apps.size() &&
            ((item.uNewState^item.uOldState)&LVIS_STATEIMAGEMASK)!=0)
            state->changed(static_cast<std::size_t>(item.iItem),(item.uNewState&LVIS_STATEIMAGEMASK)==INDEXTOSTATEIMAGEMASK(2));
    }
    return FALSE;
}
} // namespace

HMONITOR overlay_display_monitor(std::wstring_view display,HMONITOR fallback) {
    if(display.empty())return fallback;
    const auto available=displays();
    const auto found=std::find_if(available.begin(),available.end(),
        [display](const Display& entry){return entry.info.szDevice==display;});
    return found==available.end()?fallback:found->handle;
}

std::optional<OverlayScenePosition> edit_overlay_scene(HWND owner,HWND overlay,
    std::optional<OverlayScenePosition> initial,int margin) {
    Scene scene;scene.screens=displays();scene.margin=margin;
    if(scene.screens.empty())return {};
    RECT rect{};if(!GetWindowRect(overlay,&rect))return {};
    scene.size={rect.right-rect.left,rect.bottom-rect.top};
    const auto monitor=overlay_display_monitor(initial?initial->display:L"",
        MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST));
    const auto found=std::find_if(scene.screens.begin(),scene.screens.end(),
        [monitor](const Display& entry){return entry.handle==monitor;});
    if(found!=scene.screens.end())scene.screen=static_cast<std::size_t>(found-scene.screens.begin());
    const auto& work=scene.screens[scene.screen].info.rcWork;
    scene.position=initial.value_or(OverlayScenePosition{
        scene_axis_position(work.right-work.left,scene.size.cx,margin,rect.left-work.left),
        scene_axis_position(work.bottom-work.top,scene.size.cy,margin,rect.top-work.top),{}});
    if(modal(owner,scene_message,reinterpret_cast<LPARAM>(&scene),450,330)!=IDOK)return {};
    scene.position.display=scene.screens[scene.screen].info.szDevice;
    return scene.position;
}

void edit_overlay_blacklist(HWND owner,const std::vector<OverlayApplication>& apps,AppIconCache& icons,HWND& dialog,
    const std::function<void(std::size_t,bool)>& changed) {
    Blacklist state{apps,icons,dialog,changed};
    (void)modal(owner,blacklist_message,reinterpret_cast<LPARAM>(&state),430,218);
}
} // namespace fluxtop
