#include "fluxtop/control_wheel.hpp"
#include <iostream>
#include <stdexcept>
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
}
int main(){try{
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_BAR_CLASSES};check(InitCommonControlsEx(&init)!=FALSE,"initialize native controls");
    const auto parent=CreateWindowExW(0,L"STATIC",L"Wheel fixture",WS_POPUP,0,0,400,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(parent!=nullptr,"hidden parent");
    const auto slider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD,0,0,200,30,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,40,200,120,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(slider && combo,"create slider and selector");
    fluxtop::protect_setting_from_wheel(slider,TRACKBAR_CLASSW);fluxtop::protect_setting_from_wheel(combo,L"COMBOBOX");
    DWORD_PTR reference{};
    check(GetWindowSubclass(slider,fluxtop::setting_wheel_filter,1,&reference)!=FALSE &&
        GetWindowSubclass(combo,fluxtop::setting_wheel_filter,1,&reference)!=FALSE,"wheel protection installed");
    SendMessageW(slider,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));SendMessageW(slider,TBM_SETPOS,TRUE,50);
    for(const auto* text:{L"1 minute",L"5 minutes",L"30 minutes"})SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(combo,CB_SETCURSEL,1,0);
    for(const auto delta:{120,-120,240,-240})for(const auto message:{WM_MOUSEWHEEL,WM_MOUSEHWHEEL}){
        const auto wheel=MAKEWPARAM(0,static_cast<WORD>(delta));
        SendMessageW(slider,message,wheel,0);SendMessageW(combo,message,wheel,0);
        check(SendMessageW(slider,TBM_GETPOS,0,0)==50,"wheel cannot silently change a persisted slider value");
        check(SendMessageW(combo,CB_GETCURSEL,0,0)==1,"wheel cannot silently change a closed selector");
    }
    SendMessageW(combo,CB_SETCURSEL,2,0);SendMessageW(slider,TBM_SETPOS,TRUE,75);
    check(SendMessageW(combo,CB_GETCURSEL,0,0)==2 && SendMessageW(slider,TBM_GETPOS,0,0)==75,"normal setting updates still work");
    DestroyWindow(parent);std::cout<<"Native selector/slider accidental-wheel protection passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
