// Native per-user FluxTop uninstaller. Never deletes the data directory itself.
// A temporary copy performs cleanup after this image has exited; no cmd.exe or PowerShell.
#include "fluxtop/uninstall_cleanup.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <cwchar>
#include <iterator>

namespace {
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr wchar_t uninstall_key[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FluxTop";
constexpr wchar_t start_menu_marker[]=L"FluxTop installed launcher v1";
constexpr wchar_t startup_marker[]=L"FluxTop autostart v1";
constexpr wchar_t approval_key[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ComScope(){if(FAILED(result)&&result!=RPC_E_CHANGED_MODE)throw std::runtime_error("Cannot initialize COM");}
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
};
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR buffer{};
    if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&buffer)))throw std::runtime_error("Cannot resolve Windows user folder");
    fs::path path(buffer);CoTaskMemFree(buffer);return path;
}
fs::path self_path() {
    std::wstring buffer(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!n||n>=buffer.size())throw std::runtime_error("Cannot locate uninstaller executable");
    buffer.resize(n);return buffer;
}
fs::path root_path(){return known_folder(FOLDERID_LocalAppData)/L"FluxTop";}
void windows_error(const char* stage) {
    throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),stage);
}
bool registered_for(const fs::path& root) {
    wchar_t text[32768]{};DWORD bytes=sizeof(text);
    if(RegGetValueW(HKEY_CURRENT_USER,uninstall_key,L"InstallLocation",RRF_RT_REG_SZ,
                    nullptr,text,&bytes)!=ERROR_SUCCESS)return false;
    // Check registration against our fixed per-user installation folder, not argv.
    return CompareStringOrdinal(text,-1,root.c_str(),-1,TRUE)==CSTR_EQUAL;
}
void remove_shortcut_if_owned(const fs::path& path,std::wstring_view marker) {
    std::error_code ec;
    if(!fs::exists(path,ec))return;
    if(ec)throw fs::filesystem_error("Inspect shortcut",path,ec);
    ComPtr<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&link))))throw std::runtime_error("Cannot open ShellLink");
    ComPtr<IPersistFile> file;
    wchar_t description[256]{};
    if(FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(),STGM_READ)) ||
       FAILED(link->GetDescription(description,static_cast<int>(std::size(description)))) ||
       std::wstring_view(description)!=marker)return; // Never touch unrelated links.
    if(!DeleteFileW(path.c_str()))windows_error("Remove managed shortcut");
}
void unregister_windows(const fs::path& root) {
    const auto startup=known_folder(FOLDERID_Startup)/L"FluxTop.lnk";
    const auto menu=known_folder(FOLDERID_Programs)/L"FluxTop.lnk";
    // Only remove the Windows approval entry when our shortcut was actually owned.
    bool owned_startup=false;
    {
        std::error_code ec;
        if(fs::exists(startup,ec)) {
            ComPtr<IShellLinkW> link;
            if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&link)))) {
                ComPtr<IPersistFile> file;wchar_t description[256]{};
                owned_startup=SUCCEEDED(link.As(&file)) &&
                    SUCCEEDED(file->Load(startup.c_str(),STGM_READ)) &&
                    SUCCEEDED(link->GetDescription(description,static_cast<int>(std::size(description)))) &&
                    std::wstring_view(description)==startup_marker;
            }
        }
    }
    remove_shortcut_if_owned(startup,startup_marker);
    remove_shortcut_if_owned(menu,start_menu_marker);
    if(owned_startup) {
        HKEY key{};
        if(RegOpenKeyExW(HKEY_CURRENT_USER,approval_key,0,KEY_SET_VALUE,&key)==ERROR_SUCCESS) {
            (void)RegDeleteValueW(key,L"FluxTop.lnk");RegCloseKey(key);
        }
    }
    if(!registered_for(root))throw std::runtime_error("FluxTop user registration changed; refusing to remove it");
    const LONG result=RegDeleteTreeW(HKEY_CURRENT_USER,uninstall_key);
    if(result!=ERROR_SUCCESS)throw std::system_error(static_cast<int>(result),std::system_category(),"Remove FluxTop registration");
}
void delete_installed_helper(const fs::path& file) {
    for(unsigned i=0;i<40;++i){
        if(DeleteFileW(file.c_str()))return;
        const DWORD e=GetLastError();
        if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return;
        if((e!=ERROR_SHARING_VIOLATION&&e!=ERROR_ACCESS_DENIED)||i==39)windows_error("Remove installed uninstaller executable");
        Sleep(250);
    }
}
void try_delete_temporary_self() noexcept {
    const auto path=self_path();
    HANDLE file=CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags=FILE_DISPOSITION_FLAG_DELETE|FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
        if(SetFileInformationByHandle(file,FileDispositionInfoEx,&disposition,sizeof(disposition))){
            CloseHandle(file);return;
        }
        CloseHandle(file);
    }
    // Best effort only: older filesystems may not support POSIX delete of an active image.
    (void)MoveFileExW(path.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);
}
void perform_cleanup(DWORD originating_pid) {
    const auto root=root_path();
    if(!registered_for(root))throw std::runtime_error("FluxTop is not registered for this Windows user");
    if(FindWindowW(L"FluxTop.Overlay.Window",nullptr))
        throw std::runtime_error("FluxTop monitor is running. Close it from the tray first");
    HANDLE original=OpenProcess(SYNCHRONIZE,FALSE,originating_pid);
    if(original) {
        const DWORD result=WaitForSingleObject(original,30000);
        CloseHandle(original);
        if(result!=WAIT_OBJECT_0)throw std::runtime_error("Original uninstaller did not exit");
    }
    if(MessageBoxW(nullptr,L"Удалить FluxTop?\n\nИстория, настройки, база данных и журналы останутся на компьютере.",
                   L"Удаление FluxTop",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) {
        try_delete_temporary_self();
        return;
    }
    if(FindWindowW(L"FluxTop.Overlay.Window",nullptr))
        throw std::runtime_error("FluxTop monitor started while uninstall was pending");
    fluxtop::remove_installed_binaries(root);
    unregister_windows(root);
    delete_installed_helper(root/L"Uninstall-FluxTop.exe");
    MessageBoxW(nullptr,L"FluxTop удалён. История, настройки, база данных и журналы сохранены.",
                L"FluxTop — удаление",MB_OK|MB_ICONINFORMATION);
    try_delete_temporary_self();
}
fs::path temp_path() {
    wchar_t buffer[32768]{};
    const DWORD n=GetTempPathW(static_cast<DWORD>(std::size(buffer)),buffer);
    if(!n||n>=std::size(buffer))throw std::runtime_error("Cannot resolve user temporary directory");
    GUID id{};
    if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot create unique uninstall worker name");
    wchar_t guid[40]{};
    if(!StringFromGUID2(id,guid,static_cast<int>(std::size(guid))))throw std::runtime_error("Cannot format GUID");
    return fs::path(buffer)/(L"FluxTop-uninstall-"+std::wstring(guid)+L".exe");
}
void launch_worker() {
    const auto original=self_path();
    const auto temporary=temp_path();
    if(!CopyFileW(original.c_str(),temporary.c_str(),TRUE))windows_error("Stage native uninstall worker");
    const std::wstring arguments=L"\""+temporary.wstring()+L"\" --cleanup "+std::to_wstring(GetCurrentProcessId());
    auto command=arguments;
    STARTUPINFOW start{};start.cb=sizeof(start);
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(temporary.c_str(),command.data(),nullptr,nullptr,FALSE,0,
                       nullptr,temporary.parent_path().c_str(),&start,&child)) {
        const DWORD error=GetLastError();DeleteFileW(temporary.c_str());SetLastError(error);
        windows_error("Start native uninstall worker");
    }
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        ComScope com;
        int argc{};
        wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv)throw std::runtime_error("Cannot parse command line");
        struct Args {wchar_t** p;~Args(){LocalFree(p);}} args{argv};
        if(argc==3 && std::wstring_view(argv[1])==L"--cleanup") {
            // The worker can touch only the fixed LocalAppData/FluxTop directory.
            // Disallow launching cleanup from an arbitrary executable path.
            const auto self=self_path();
            const auto temp=fs::weakly_canonical(fs::temp_directory_path());
            if(fs::weakly_canonical(self.parent_path())!=temp ||
               !self.filename().wstring().starts_with(L"FluxTop-uninstall-") ||
               self.extension()!=L".exe")
                throw std::runtime_error("Untrusted uninstall worker location");
            wchar_t* end{};const unsigned long pid=std::wcstoul(argv[2],&end,10);
            if(!end||*end||!pid)throw std::runtime_error("Invalid uninstall parent PID");
            perform_cleanup(static_cast<DWORD>(pid));
            return 0;
        }
        if(argc!=1)throw std::runtime_error("Unsupported uninstall arguments");
        const auto root=root_path();
        if(!registered_for(root))throw std::runtime_error("FluxTop is not registered for this Windows user");
        if(FindWindowW(L"FluxTop.Overlay.Window",nullptr)) {
            MessageBoxW(nullptr,L"Сначала полностью закройте FluxTop через значок в системном трее.",
                        L"FluxTop — удаление",MB_OK|MB_ICONINFORMATION);
            return 0;
        }
        launch_worker();
        return 0;
    }catch(const std::exception& error){
        const std::string msg=error.what();
        const int count=MultiByteToWideChar(CP_UTF8,0,msg.c_str(),-1,nullptr,0);
        std::wstring message(static_cast<std::size_t>(count>0?count:1),L'\0');
        if(count>0)MultiByteToWideChar(CP_UTF8,0,msg.c_str(),-1,message.data(),count);
        MessageBoxW(nullptr,(std::wstring(L"Не удалось удалить FluxTop.\n\n")+message.c_str()).c_str(),
                    L"FluxTop — ошибка удаления",MB_OK|MB_ICONERROR);
        return 1;
    }
}
