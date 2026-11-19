#include "fluxtop/installation.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/release_update.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fluxtop {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t uninstall_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FluxTop";
constexpr wchar_t start_menu_marker[] = L"FluxTop installed launcher v1";
constexpr wchar_t startup_marker[] = L"FluxTop autostart v1";
constexpr wchar_t startup_approval[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(id,0,nullptr,&value)))
        throw std::runtime_error("Cannot resolve a Windows shell folder");
    const std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}
std::filesystem::path start_menu_link() {
    return known_folder(FOLDERID_Programs) / L"FluxTop.lnk";
}
std::filesystem::path startup_link() {
    return known_folder(FOLDERID_Startup) / L"FluxTop.lnk";
}
ComPtr<IShellLinkW> new_link() {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))))
        throw std::runtime_error("ShellLink COM initialization failed");
    return link;
}
bool owned_shortcut(const std::filesystem::path& path, std::wstring_view marker) {
    if (!std::filesystem::exists(path)) return false;
    const auto link = new_link();
    ComPtr<IPersistFile> file;
    wchar_t description[256]{};
    return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(path.c_str(),STGM_READ))
        && SUCCEEDED(link->GetDescription(description,static_cast<int>(std::size(description))))
        && std::wstring_view(description) == marker;
}
void make_shortcut(const std::filesystem::path& target, const std::filesystem::path& shortcut) {
    if (std::filesystem::exists(shortcut) && !owned_shortcut(shortcut,start_menu_marker))
        throw std::runtime_error("Start Menu shortcut FluxTop.lnk exists but is not owned by FluxTop");
    const auto link = new_link();
    if (FAILED(link->SetPath(target.c_str()))
        || FAILED(link->SetWorkingDirectory(target.parent_path().c_str()))
        || FAILED(link->SetDescription(start_menu_marker))
        || FAILED(link->SetIconLocation(target.c_str(),0)))
        throw std::runtime_error("Cannot configure Start Menu shortcut");
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) throw std::runtime_error("Cannot save Start Menu shortcut");
    const auto temporary = shortcut.wstring() + L".pending";
    if (FAILED(file->Save(temporary.c_str(),TRUE)))
        throw std::runtime_error("Cannot write Start Menu shortcut");
    if (!MoveFileExW(temporary.c_str(),shortcut.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(code);
        throw windows_error("Publish Start Menu shortcut");
    }
}
// Retarget only an existing, FluxTop-owned Startup shortcut; preserve the user's
// enabled/disabled setting, including Explorer StartupApproved.
void migrate_owned_startup(const std::filesystem::path& installed) {
    const auto shortcut=startup_link();
    if (!owned_shortcut(shortcut,startup_marker)) return;
    const auto link=new_link();
    if (FAILED(link->SetPath(installed.c_str())) || FAILED(link->SetArguments(L"--autostart"))
        || FAILED(link->SetWorkingDirectory(installed.parent_path().c_str()))
        || FAILED(link->SetDescription(startup_marker)))
        throw std::runtime_error("Cannot retarget FluxTop Startup shortcut");
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist))) throw std::runtime_error("Cannot save FluxTop Startup shortcut");
    const auto temporary=shortcut.wstring()+L".pending";
    if (FAILED(persist->Save(temporary.c_str(),TRUE)))
        throw std::runtime_error("Cannot update FluxTop Startup shortcut");
    if (!MoveFileExW(temporary.c_str(),shortcut.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(code);
        throw windows_error("Publish updated Startup shortcut");
    }
}
void write_registry_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    if (RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot write installed application registry entry");
}
void register_program(const std::filesystem::path& target, std::wstring_view tag) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER,uninstall_key,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot register FluxTop for the current user");
    struct Closer { HKEY key; ~Closer() { if(key) RegCloseKey(key); } } close{key};
    const std::wstring version(tag.starts_with(L"v") ? tag.substr(1) : tag);
    write_registry_string(key,L"DisplayName",L"FluxTop");
    write_registry_string(key,L"DisplayVersion",version);
    write_registry_string(key,L"Publisher",L"Dilichan");
    write_registry_string(key,L"InstallLocation",target.parent_path().wstring());
    write_registry_string(key,L"DisplayIcon",L"\""+target.wstring()+L"\",0");
    write_registry_string(key,L"UninstallString",L"\""+target.wstring()+L"\" --uninstall");
    write_registry_string(key,L"URLInfoAbout",L"https://github.com/Dilichan/FluxTop");
    constexpr DWORD one=1;
    (void)RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
    (void)RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
}
void copy_if_needed(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (same_file(source,target)) return;
    // Older downloaded launchers cannot overwrite a newer installed version offline.
    const auto existing = launcher_release_tag(target);
    const auto incoming = launcher_release_tag(source);
    if (!existing.empty() && !incoming.empty() && newer_release(existing,incoming)) return;
    const auto pending=target.wstring()+L".install."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    if (!CopyFileW(source.c_str(),pending.c_str(),FALSE)) throw windows_error("Copy FluxTop installer");
    if (!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError(); DeleteFileW(pending.c_str()); SetLastError(code);
        throw windows_error("Activate installed launcher");
    }
}
// A batch file is used only for final deletion because an executing Windows PE
// cannot reliably delete its own image. Its fixed script does not delete user data.
void make_uninstall_cleanup(const std::filesystem::path& directory) {
    constexpr char script[] =
        "@echo off\r\n"
        "setlocal EnableExtensions DisableDelayedExpansion\r\n"
        "set /a attempts=0\r\n"
        ":retry\r\n"
        "del /f /q \"%~dp0FluxTop.exe\" >nul 2>&1\r\n"
        "if not exist \"%~dp0FluxTop.exe\" goto done\r\n"
        "set /a attempts+=1\r\n"
        "if %attempts% GEQ 20 exit /b 1\r\n"
        "ping -n 2 127.0.0.1 >nul\r\n"
        "goto retry\r\n"
        ":done\r\n"
        "if exist \"%~dp0versions\" rmdir /s /q \"%~dp0versions\"\r\n"
        "del /f /q \"%~f0\" >nul 2>&1\r\n";
    const auto path=directory/L"Uninstall-FluxTop.cmd";
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    if (!file.valid()) throw windows_error("Write uninstall helper");
    DWORD written{};
    constexpr DWORD length=static_cast<DWORD>(sizeof(script)-1);
    if (!WriteFile(file.get(),script,length,&written,nullptr) || written!=length)
        throw windows_error("Save uninstall helper");
}
void remove_our_shortcut(const std::filesystem::path& path, std::wstring_view marker) {
    if (owned_shortcut(path,marker) && !DeleteFileW(path.c_str()))
        throw windows_error("Remove FluxTop shortcut");
}
} // namespace

bool monitor_running() noexcept {
    return FindWindowW(L"FluxTop.Overlay.Window",nullptr)!=nullptr;
}
std::wstring launcher_release_tag(const std::filesystem::path& launcher) {
    const HMODULE module=LoadLibraryExW(launcher.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return {};
    wchar_t buffer[128]{};
    const int count=LoadStringW(module,203,buffer,static_cast<int>(std::size(buffer)));
    FreeLibrary(module);
    return count>0 ? std::wstring(buffer,static_cast<std::size_t>(count)) : std::wstring{};
}
bool same_file(const std::filesystem::path& a,const std::filesystem::path& b) noexcept {
    std::error_code error;
    return std::filesystem::equivalent(a,b,error) && !error;
}
bool application_registered(const std::filesystem::path& install_path) {
    wchar_t buffer[32768]{};
    DWORD length=sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER,uninstall_key,L"InstallLocation",RRF_RT_REG_SZ,
        nullptr,buffer,&length)!=ERROR_SUCCESS) return false;
    const auto registered_path=std::filesystem::path(buffer)/L"FluxTop.exe";
    return same_file(registered_path,install_path);
}
bool installation_version_current(std::wstring_view tag) {
    wchar_t buffer[128]{};
    DWORD bytes=sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER,uninstall_key,L"DisplayVersion",RRF_RT_REG_SZ,
        nullptr,buffer,&bytes)!=ERROR_SUCCESS) return false;
    return std::wstring_view(buffer) == (tag.starts_with(L"v") ? tag.substr(1) : tag);
}
void install_application(const std::filesystem::path& source,
    const std::filesystem::path& data_directory, std::wstring_view tag, bool register_windows) {
    std::filesystem::create_directories(data_directory);
    const auto installed=data_directory/L"FluxTop.exe";
    diagnostics::write(diagnostics::Level::info,"install.begin","application installation requested");
    // For fixture tests, install the binary only and never alter real user shell state.
    copy_if_needed(source,installed);
    if (!register_windows) return;
    make_shortcut(installed,start_menu_link());
    // A restricted/locked Startup shortcut must not make installation fail.
    // Keep the original shortcut intact and report how to repair its target.
    bool startup_warning=false;
    try { migrate_owned_startup(installed); }
    catch (const std::exception& error) {
        startup_warning=true;
        diagnostics::write(diagnostics::Level::warning,"install.startup",error.what());
    }
    make_uninstall_cleanup(data_directory);
    const auto installed_tag=launcher_release_tag(installed);
    register_program(installed,installed_tag.empty() ? tag : std::wstring_view(installed_tag));
    if (startup_warning) MessageBoxW(nullptr,
        L"FluxTop установлен, но Windows запретила обновить старый ярлык автозапуска. "
        L"Выключите и снова включите автозапуск в настройках FluxTop. Подробности: activity.log.",
        L"FluxTop — автозапуск",MB_OK|MB_ICONWARNING);
}
void launch_installed_application(const std::filesystem::path& data_directory,bool tray) {
    const auto path=data_directory/L"FluxTop.exe";
    SHELLEXECUTEINFOW start{};
    start.cbSize=sizeof(start);
    start.lpFile=path.c_str();
    start.lpParameters=tray ? L"--autostart" : nullptr;
    start.nShow=SW_SHOWNORMAL;
    if (!ShellExecuteExW(&start)) throw windows_error("Launch installed FluxTop");
}
void uninstall_application(const std::filesystem::path& data_directory) {
    const auto installed=data_directory/L"FluxTop.exe";
    if (!application_registered(installed))
        throw std::runtime_error("FluxTop is not registered for this Windows user");
    if (monitor_running()) throw std::runtime_error("Close FluxTop from the system tray before uninstalling");
    const auto helper=data_directory/L"Uninstall-FluxTop.cmd";
    if (!std::filesystem::exists(helper)) make_uninstall_cleanup(data_directory);
    // Only shortcuts bearing FluxTop's ownership markers are removed.
    const bool had_owned_startup=owned_shortcut(startup_link(),startup_marker);
    remove_our_shortcut(startup_link(),startup_marker);
    remove_our_shortcut(start_menu_link(),start_menu_marker);
    HKEY key{};
    if (had_owned_startup && RegOpenKeyExW(HKEY_CURRENT_USER,startup_approval,0,KEY_SET_VALUE,&key)==ERROR_SUCCESS) {
        (void)RegDeleteValueW(key,L"FluxTop.lnk"); RegCloseKey(key);
    }
    if (RegDeleteTreeW(HKEY_CURRENT_USER,uninstall_key)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot remove FluxTop installed-app registration");
    // Run a fixed, user-owned helper after this EXE exits. It removes only app
    // binaries; fluxtop.db, overlay.ini and diagnostics are preserved.
    std::wstring cmd=L"/d /c \"\""+helper.wstring()+L"\"\"";
    SHELLEXECUTEINFOW start{};
    start.cbSize=sizeof(start);
    start.lpFile=L"cmd.exe";
    start.lpParameters=cmd.c_str();
    start.nShow=SW_HIDE;
    if (!ShellExecuteExW(&start)) throw windows_error("Start FluxTop removal helper");
}
} // namespace fluxtop
