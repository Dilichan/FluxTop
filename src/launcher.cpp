#include "fluxtop/windows.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/release_update.hpp"
#include "fluxtop/installation.hpp"
#include "fluxtop/pawnio_bundle.hpp"
#include "fluxtop/launcher_lock.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>

namespace {
std::filesystem::path executable() {
    std::wstring path(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!size || size>=path.size()) throw fluxtop::windows_error("Locate launcher");
    path.resize(size); return path;
}
std::filesystem::path data_directory() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder)))
        throw std::runtime_error("Cannot locate LocalAppData");
    std::filesystem::path result(folder); CoTaskMemFree(folder); return result/L"FluxTop";
}
bool matches(const std::filesystem::path& path,const BYTE* bytes,DWORD size) {
    fluxtop::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    if (!file.valid()) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(),&length) || length.QuadPart!=size) return false;
    std::array<BYTE,65536> buffer{};
    DWORD offset=0;
    while (offset<size) {
        const auto count=static_cast<DWORD>(std::min<std::size_t>(buffer.size(),size-offset));
        DWORD read{};
        if (!ReadFile(file.get(),buffer.data(),count,&read,nullptr) || read!=count
            || std::memcmp(buffer.data(),bytes+offset,count)!=0) return false;
        offset+=count;
    }
    return true;
}
void publish(const std::filesystem::path& destination,const BYTE* bytes,DWORD size) {
    if (matches(destination,bytes,size)) return;
    const auto temporary=destination.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    {
        fluxtop::Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if (!file.valid()) throw fluxtop::windows_error("Create monitor cache");
        DWORD written{};
        if (!WriteFile(file.get(),bytes,size,&written,nullptr) || written!=size || !FlushFileBuffers(file.get()))
            throw fluxtop::windows_error("Write monitor cache");
    }
    if (!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(code);
        throw fluxtop::windows_error("Publish monitor cache");
    }
}
void cache_launcher(const std::filesystem::path& directory) {
    const auto source=executable();
    const auto destination=directory/L"FluxTop.exe";
    std::error_code error;
    if (std::filesystem::equivalent(source,destination,error)) return;
    const auto temporary=directory/(L"launcher-"+std::to_wstring(GetCurrentProcessId())+L".tmp");
    if (!CopyFileW(source.c_str(),temporary.c_str(),FALSE)) throw fluxtop::windows_error("Cache launcher");
    if (!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(code);
        throw fluxtop::windows_error("Publish launcher cache");
    }
}
// The GUI launcher requests UAC only for installation/registration.
// Normal starts and fixture tests keep their existing elevation behavior.
bool installer_is_elevated() {
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))
        throw fluxtop::windows_error("Inspect launcher token");
    fluxtop::Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD bytes{};
    if (!GetTokenInformation(token.get(),TokenElevation,&elevation,sizeof(elevation),&bytes))
        throw fluxtop::windows_error("Read launcher elevation");
    return elevation.TokenIsElevated != FALSE;
}
void request_install_elevation(const std::filesystem::path& path) {
    SHELLEXECUTEINFOW command{};
    command.cbSize=sizeof(command);
    command.lpVerb=L"runas";
    command.lpFile=path.c_str();
    command.lpParameters=L"--install-and-launch";
    command.nShow=SW_SHOWNORMAL;
    if (!ShellExecuteExW(&command)) {
        const auto error=GetLastError();
        if (error==ERROR_CANCELLED) return; // User declined UAC: no installation.
        SetLastError(error);
        throw fluxtop::windows_error("Request administrator rights for installation");
    }
}
// The monitor handles WM_CLOSE by saving settings and shutting down its worker.
// Wait for the process to exit before replacing the installed launcher.
bool close_monitor_for_consented_update() {
    const auto window=FindWindowW(L"FluxTop.Overlay.Window",nullptr);
    if (!window) return true;
    DWORD pid{};
    GetWindowThreadProcessId(window,&pid);
    if (!pid) return false;
    fluxtop::Handle process(OpenProcess(SYNCHRONIZE,FALSE,pid));
    if (!process.valid()) return false;
    if (!PostMessageW(window,WM_CLOSE,0,0)) return !fluxtop::monitor_running();
    return WaitForSingleObject(process.get(),20000)==WAIT_OBJECT_0;
}
}
int WINAPI wWinMain(HINSTANCE module,HINSTANCE,PWSTR,int) {
    if(std::wstring_view(GetCommandLineW()).find(L"--pawnio-dry-run")==std::wstring_view::npos)
        fluxtop::diagnostics::write(fluxtop::diagnostics::Level::info,"launcher.start","bootstrap started");
    bool prepare=false,install_only=false;
    try {
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if (FAILED(com) && com!=RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot initialize launcher COM apartment");
        struct ComScope { bool initialized; ~ComScope() { if (initialized) CoUninitialize(); } } com_scope{SUCCEEDED(com)};
        int count{}; auto** argv=CommandLineToArgvW(GetCommandLineW(),&count);
        if (!argv) throw fluxtop::windows_error("Read launcher arguments");
        struct Arguments { wchar_t** value; ~Arguments(){ LocalFree(value); } } arguments{argv};
        bool tray=false,autostart=false,uninstall=false,custom_directory=false,install_and_launch=false,check_update=false,pawnio_dry_run=false;
        std::filesystem::path directory;
        for (int i=1;i<count;++i) {
            const std::wstring_view arg(argv[i]);
            if (arg==L"--tray") tray=true;
            else if (arg==L"--autostart") { autostart=true; tray=true; }
            else if (arg==L"--prepare-only") prepare=true;
            else if (arg==L"--install-only") install_only=true;
            else if (arg==L"--install-and-launch") install_and_launch=true;
            else if (arg==L"--check-update") check_update=true;
            else if (arg==L"--uninstall") uninstall=true;
            else if (arg==L"--pawnio-dry-run") pawnio_dry_run=true;
            else if (arg==L"--data-dir" && i+1<count) {
                custom_directory=true;
                directory=std::filesystem::absolute(argv[++i]);
            }
            else throw std::runtime_error("Supported options: --tray, --prepare-only, --install-only, --uninstall, --check-update, --data-dir PATH");
        }

        // Side-effect-free offline verifier: no writes, UAC, driver install, or
        // changes to existing FluxTop registration. Returns an exit code only.
        if (pawnio_dry_run) {
            if (tray || autostart || uninstall || install_only || install_and_launch ||
                prepare || check_update || custom_directory)
                return 2;
            return fluxtop::pawnio_bundle_verified(executable()) ? 0 : 3;
        }
        fluxtop::diagnostics::write(fluxtop::diagnostics::Level::info,"launcher.options",
            "tray="+std::to_string(tray)+" autostart="+std::to_string(autostart)+
            " install_only="+std::to_string(install_only)+" uninstall="+std::to_string(uninstall));
        if (directory.empty()) directory=data_directory();
        std::filesystem::create_directories(directory);
        // Startup's grace period must not monopolize the publication lock.
        if (autostart && !prepare) Sleep(30000);
        // Keep download/activation and local-cache publication serialized across launches.
        const auto lock_name=fluxtop::launcher_lock_name(directory,custom_directory);
        fluxtop::Handle startup(CreateMutexW(nullptr,FALSE,lock_name.c_str()));
        if (!startup.valid()) throw fluxtop::windows_error("Create startup lock");
        const auto wait=WaitForSingleObject(startup.get(),3000);
        if (wait==WAIT_TIMEOUT) {
            fluxtop::diagnostics::write(fluxtop::diagnostics::Level::warning,"launcher.busy","bootstrap lock timed out; no installation or launch performed");
            return ERROR_BUSY;
        }
        if (wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) throw fluxtop::windows_error("Wait for startup lock");
        struct Unlock { HANDLE value; ~Unlock(){ ReleaseMutex(value); } } unlock{startup.get()};
        wchar_t tag[128]{};
        if (!LoadStringW(module,203,tag,static_cast<int>(std::size(tag)))) throw std::runtime_error("Release version missing");
        const auto self=executable();
        const auto installed=directory/L"FluxTop.exe";
        if (check_update) {
            // Only the installed launcher may run a check. No check on normal start,
            // autostart, installation, or CI fixture preparation.
            if (prepare || install_only || install_and_launch || uninstall || autostart
                || custom_directory || !fluxtop::same_file(self,installed)) return 0;
            bool approved=false;
            try {
                const auto candidate=fluxtop::find_release_update(tag);
                if (!candidate) return 0; // No update: no prompt and no download.
                const auto question=std::wstring(L"Доступно обновление FluxTop ") + candidate->tag
                    + L".\n\nСкачать и установить его сейчас?";
                if (MessageBoxW(nullptr,question.c_str(),L"FluxTop — обновление",
                    MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2|MB_TOPMOST)!=IDYES) return 0;
                approved=true;
                // Download and verify BEFORE closing the running monitor.
                const auto bytes=fluxtop::download_release_update(*candidate);
                if (bytes.size()!=candidate->size || bytes.size()<1024
                    || bytes[0]!='M' || bytes[1]!='Z'
                    || fluxtop::release_sha256(bytes)!=candidate->digest)
                    throw std::runtime_error("Downloaded update failed SHA-256, size or PE validation");
                if (!close_monitor_for_consented_update() || fluxtop::monitor_running())
                    throw std::runtime_error("Cannot close the current monitor for update");
                const auto launch_new=[](const std::filesystem::path& path) {
                    SHELLEXECUTEINFOW start{};
                    start.cbSize=sizeof(start);
                    start.lpFile=path.c_str();
                    start.nShow=SW_SHOWNORMAL;
                    return ShellExecuteExW(&start)!=FALSE;
                };
                (void)fluxtop::activate_release(directory,bytes,*candidate,launch_new);
                return 0;
            } catch (const std::exception& error) {
                fluxtop::diagnostics::write(fluxtop::diagnostics::Level::error,"update.failure",error.what());
                if (approved) {
                    // If activation failed after shutdown, restore the old app in tray mode.
                    if (!fluxtop::monitor_running() && std::filesystem::exists(installed)) {
                        SHELLEXECUTEINFOW restart{};
                        restart.cbSize=sizeof(restart);
                        restart.lpFile=installed.c_str();
                        restart.lpParameters=L"--tray";
                        restart.nShow=SW_SHOWNORMAL;
                        (void)ShellExecuteExW(&restart);
                    }
                    MessageBoxW(nullptr,
                        L"Не удалось применить обновление FluxTop. Подробности: %LOCALAPPDATA%\\FluxTop\\activity.log.",
                        L"FluxTop — обновление",MB_OK|MB_ICONERROR);
                }
                return 1;
            }
        }
        if (uninstall) {
            if (custom_directory || !fluxtop::same_file(self,installed))
                throw std::runtime_error("Run --uninstall from the installed FluxTop.exe");
            fluxtop::uninstall_application(directory);
            return 0;
        }
        // Isolated installations copy files and leave any real monitor running.
        if (!prepare && !(install_only && custom_directory) && fluxtop::monitor_running()) {
            if (!autostart) MessageBoxW(nullptr,
                L"FluxTop уже запущен. Откройте настройки через значок в трее или Ctrl+Alt+F9.",
                L"FluxTop",MB_OK|MB_ICONINFORMATION);
            return 0;
        }
        if (install_and_launch) {
            if (custom_directory || !installer_is_elevated())
                throw std::runtime_error("Installation requires an elevated launcher and the default user directory");
            fluxtop::install_application(self,directory,tag,true);
            fluxtop::launch_installed_application(directory,false);
            return 0;
        }
        if (install_only) {
            // --data-dir fixture runs are intentionally unelevated and isolated.
            if (!custom_directory && !installer_is_elevated()) {
                request_install_elevation(self);
                return 0;
            }
            fluxtop::install_application(self,directory,tag,!custom_directory);
            return 0;
        }
        if (!prepare && !custom_directory) {
            if (!fluxtop::same_file(self,installed)) {
                const bool registered=fluxtop::application_registered(installed);
                const auto installed_tag=fluxtop::launcher_release_tag(installed);
                const bool upgrade=installed_tag.empty() || fluxtop::newer_release(tag,installed_tag);
                const wchar_t* prompt = !registered
                    ? L"Установить FluxTop для текущего пользователя? Будет установлен необходимый подписанный компонент мониторинга оборудования; программа появится в меню «Пуск» и списке установленных приложений."
                    : upgrade
                        ? L"FluxTop уже установлен. Обновить установленную версию и запустить?"
                        : L"FluxTop уже установлен. Запустить установленную копию?";
                if (MessageBoxW(nullptr,prompt,L"FluxTop — установка",MB_YESNO|MB_ICONQUESTION)!=IDYES)
                    return 0;
                if (!registered || upgrade) {
                    if (!installer_is_elevated()) {
                        request_install_elevation(self);
                        return 0;
                    }
                    fluxtop::install_application(self,directory,tag,true);
                }
                fluxtop::launch_installed_application(directory,false);
                return 0;
            }
            if (!fluxtop::application_registered(installed)) {
                // Never trigger an installation dialog during Startup/tray launch.
                if (!autostart && !tray) {
                    if (MessageBoxW(nullptr,
                        L"Добавить FluxTop в меню «Пуск» и список установленных приложений?",
                        L"FluxTop — завершение установки",MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
                    if (!installer_is_elevated()) {
                        request_install_elevation(self);
                        return 0;
                    }
                    fluxtop::install_application(self,directory,tag,true);
                }
            } else if (!fluxtop::installation_version_current(tag) && !autostart && !tray) {
                // Refresh Installed Apps metadata with user-approved UAC.
                if (!installer_is_elevated()) {
                    request_install_elevation(self);
                    return 0;
                }
                fluxtop::install_application(self,directory,tag,true);
            }
        }
        // Online updates are never downloaded or installed during regular launch.
        const auto resource=FindResourceW(module,MAKEINTRESOURCEW(201),RT_RCDATA);
        if (!resource) throw fluxtop::windows_error("Locate embedded monitor");
        const auto loaded=LoadResource(module,resource);
        const auto* bytes=static_cast<const BYTE*>(LockResource(loaded));
        const auto size=SizeofResource(module,resource);
        wchar_t hash[80]{};
        if (!bytes || !size || LoadStringW(module,202,hash,static_cast<int>(std::size(hash)))!=64)
            throw std::runtime_error("Invalid embedded monitor");
        const auto version=directory/L"versions"/hash;
        std::filesystem::create_directories(version);
        const auto monitor=version/L"fluxtop.exe";
        publish(monitor,bytes,size);
        // These modules use the existing optional PawnIO driver.
        for(const auto& [id,filename]:{std::pair{206,L"IntelMSR.bin"},std::pair{208,L"AMDFamily17.bin"}}) {
            if(const auto cpu_resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA)) {
                const auto content=LoadResource(module,cpu_resource);
                const auto* data=content?static_cast<const BYTE*>(LockResource(content)):nullptr;
                const auto cpu_size=SizeofResource(module,cpu_resource);
                if(data && cpu_size>0 && cpu_size<=65536)publish(version/filename,data,cpu_size);
            }
        }
        cache_launcher(directory);
        if (prepare) return 0;
        SHELLEXECUTEINFOW start{}; start.cbSize=sizeof(start); start.fMask=SEE_MASK_NOCLOSEPROCESS;
        start.lpVerb=L"runas"; start.lpFile=monitor.c_str(); start.lpDirectory=directory.c_str();
        start.lpParameters=tray ? L"--overlay --tray" : L"--overlay"; start.nShow=SW_HIDE;
        fluxtop::diagnostics::write(fluxtop::diagnostics::Level::info,"launcher.monitor","elevated monitor launch requested");
        if (!ShellExecuteExW(&start)) {
            const auto code=GetLastError();
            if (code==ERROR_CANCELLED) return ERROR_CANCELLED;
            throw fluxtop::windows_error("Start FluxTop monitor");
        }
        if (start.hProcess) CloseHandle(start.hProcess);
        return 0;
    } catch (const std::exception& error) {
        fluxtop::diagnostics::write(fluxtop::diagnostics::Level::error,"launcher.failure",error.what());
        if (!prepare && !install_only) {
            const auto size=MultiByteToWideChar(CP_UTF8,0,error.what(),-1,nullptr,0);
            std::wstring message(static_cast<std::size_t>(std::max(1,size)),L'\0');
            MultiByteToWideChar(CP_UTF8,0,error.what(),-1,message.data(),size);
            MessageBoxW(nullptr,(L"Не удалось запустить FluxTop.\n\n"+message).c_str(),L"FluxTop",MB_OK|MB_ICONERROR);
        }
        return 1;
    }
}
