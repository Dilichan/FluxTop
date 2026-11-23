#pragma once
#include <windows.h>
#include <commctrl.h>

namespace fluxtop {
// Wheel movement over a closed selector/slider must not silently change a
// persisted setting. Explicitly opened combo lists keep their native scrolling.
inline LRESULT CALLBACK setting_wheel_filter(HWND window,UINT message,WPARAM wp,LPARAM lp,
    UINT_PTR id,DWORD_PTR) {
    if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL){
        wchar_t kind[32]{};GetClassNameW(window,kind,32);
        if(lstrcmpiW(kind,L"COMBOBOX")!=0 || !SendMessageW(window,CB_GETDROPPEDSTATE,0,0))return 0;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,setting_wheel_filter,id);
    return DefSubclassProc(window,message,wp,lp);
}
inline void protect_setting_from_wheel(HWND window,const wchar_t* kind){
    if(window && (lstrcmpiW(kind,L"COMBOBOX")==0 || lstrcmpiW(kind,TRACKBAR_CLASSW)==0))
        SetWindowSubclass(window,setting_wheel_filter,1,0);
}
}
