#include "fluxtop/source_checkout.hpp"
#include "fluxtop/autostart.hpp"
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
struct Handle {HANDLE h{INVALID_HANDLE_VALUE};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
fs::path self(){
    std::wstring text(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,text.data(),static_cast<DWORD>(text.size()));
    if(!n||n>=text.size())throw std::runtime_error("Cannot locate source launcher executable");
    text.resize(n);return text;
}
struct Arguments {
    fs::path repository;
    bool no_update{},prepare_only{},standard_user{},autostart{},migrate_only{};
    std::vector<std::wstring> monitor;
};
Arguments parse(int argc,wchar_t** argv){
    Arguments args;
    for(int i=1;i<argc;++i){
        const std::wstring_view a(argv[i]);
        if(a==L"--repo"&&i+1<argc)args.repository=fs::absolute(argv[++i]);
        else if(a==L"--no-update"||a==L"-NoUpdate")args.no_update=true;
        else if(a==L"--prepare-only"||a==L"-PrepareOnly")args.prepare_only=true;
        else if(a==L"--standard-user"||a==L"-StandardUser")args.standard_user=true;
        else if(a==L"--autostart"||a==L"-Autostart")args.autostart=true;
        else if(a==L"--migrate-autostart-only")args.migrate_only=true;
        else args.monitor.emplace_back(a);
    }
    if(args.repository.empty())args.repository=fluxtop::source_checkout::locate_repository(self());
    return args;
}
int launch_monitor(const fs::path& binary,const fs::path& cwd,const Arguments& args){
    std::vector<std::wstring> monitor=args.autostart?
        std::vector<std::wstring>{L"--overlay",L"--tray"}:args.monitor;
    std::wstring parameters;
    for(const auto& item:monitor){if(!parameters.empty())parameters+=L' ';
        parameters+=fluxtop::source_checkout::quote(item);}
    if(args.standard_user){
        std::wstring command=fluxtop::source_checkout::quote(binary.wstring());
        if(!parameters.empty()){command+=L' ';command+=parameters;}
        STARTUPINFOW start{sizeof(start)};
        PROCESS_INFORMATION process{};
        if(!CreateProcessW(binary.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,
                           cwd.c_str(),&start,&process))return 1;
        CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
    }
    SHELLEXECUTEINFOW shell{};
    shell.cbSize=sizeof(shell);shell.fMask=SEE_MASK_NOCLOSEPROCESS;
    shell.lpFile=binary.c_str();shell.lpDirectory=cwd.c_str();
    shell.lpVerb=L"runas";
    shell.lpParameters=parameters.empty()?nullptr:parameters.c_str();
    shell.nShow=args.autostart?SW_HIDE:SW_SHOWNORMAL;
    if(!ShellExecuteExW(&shell)){
        const DWORD error=GetLastError();
        return error==ERROR_CANCELLED?1223:1;
    }
    if(shell.hProcess)CloseHandle(shell.hProcess);
    return 0;
}
int run(int argc,wchar_t** argv){
    const auto args=parse(argc,argv);
    if(args.migrate_only){
        const HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(result)&&result!=RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot initialize ShellLink COM for autostart migration");
        struct Scope{bool active;~Scope(){if(active)CoUninitialize();}} scope{SUCCEEDED(result)};
        if(fluxtop::legacy_autostart_pending()){
            fluxtop::migrate_autostart_if_needed();
            if(fluxtop::legacy_autostart_pending())
                throw std::runtime_error("Legacy startup shortcut could not be migrated; old VBS launcher retained");
        }
        return 0;
    }
    const auto state=args.repository/L"out"/L"launcher";
    fs::create_directories(state);
    if(args.autostart)Sleep(30000);
    // A share-mode-zero open serializes update, build, and manifest publication.
    const auto lock=state/L"update.lock";
    Handle owned{CreateFileW(lock.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(!owned.h||owned.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Checkout launcher already active");
    const auto exe=fluxtop::source_checkout::prepare(args.repository,state,args.no_update,args.prepare_only);
    CloseHandle(owned.h);owned.h=INVALID_HANDLE_VALUE;
    if(args.prepare_only)return 0;
    return launch_monitor(exe,args.repository,args);
}
int main_w(int argc,wchar_t** argv){
    try{return run(argc,argv);}
    catch(const std::exception& e){
#ifdef FLUXTOP_SOURCE_GUI
        try{
            const auto root=fluxtop::source_checkout::locate_repository(self());
            const auto state=root/L"out"/L"launcher";
            fs::create_directories(state);
            std::ofstream out(state/L"autostart.log",std::ios::app);
            out<<"Source autostart failed: "<<e.what()<<'\n';
        }catch(...){ }
#else
        std::cerr<<"FluxTop source launcher: "<<e.what()<<'\n';
#endif
        return 1;
    }
}
}
#ifdef FLUXTOP_SOURCE_GUI
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int argc{};wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 1;
    const int code=main_w(argc,argv);
    LocalFree(argv);
    return code;
}
#else
int wmain(int argc,wchar_t** argv){return main_w(argc,argv);}
#endif
