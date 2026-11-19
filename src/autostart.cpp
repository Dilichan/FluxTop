#include "fluxtop/autostart.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>

namespace fluxtop {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t description[] = L"FluxTop autostart v1";
constexpr wchar_t approved_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
std::filesystem::path shortcut_path() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_DEFAULT, nullptr, &folder)))
        throw std::runtime_error("Cannot locate the current user's Startup folder");
    std::filesystem::path result(folder); CoTaskMemFree(folder);
    return result / L"FluxTop.lnk";
}
ComPtr<IShellLinkW> shell_link() {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        throw std::runtime_error("Cannot create a startup shortcut");
    return link;
}
bool owned(IShellLinkW* link, const std::filesystem::path& path) {
    ComPtr<IPersistFile> file;
    wchar_t text[128]{};
    return SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))
        && SUCCEEDED(file->Load(path.c_str(), STGM_READ))
        && SUCCEEDED(link->GetDescription(text, static_cast<int>(std::size(text))))
        && std::wstring_view(text) == description;
}
std::filesystem::path bootstrap() {
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size >= executable.size()) throw windows_error("Locate FluxTop executable");
    executable.resize(size);
    auto folder = std::filesystem::path(executable).parent_path();
    while (!folder.empty()) {
        const auto script = folder / L"tools" / L"Start-FluxTop.vbs";
        if (std::filesystem::exists(script) && std::filesystem::exists(folder / L"fluxtop.cmd")) return script;
        const auto parent = folder.parent_path();
        if (parent == folder) break;
        folder = parent;
    }
    PWSTR local{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))) {
        const auto portable=std::filesystem::path(local)/L"FluxTop"/L"FluxTop.exe";
        CoTaskMemFree(local);
        if (std::filesystem::exists(portable)) return portable;
    }
    throw std::runtime_error("Cannot find the FluxTop launcher. Start the downloaded FluxTop.exe first.");
}
bool disabled_by_windows() {
    BYTE data[32]{}; DWORD bytes = sizeof(data);
    return RegGetValueW(HKEY_CURRENT_USER, approved_key, L"FluxTop.lnk", RRF_RT_REG_BINARY, nullptr, data, &bytes) == ERROR_SUCCESS
        && bytes && (data[0] & 1);
}
}
bool autostart_enabled() {
    const auto path = shortcut_path();
    if (!std::filesystem::exists(path)) return false;
    const auto link = shell_link();
    return owned(link.Get(), path) && !disabled_by_windows();
}
void set_autostart(bool enabled) {
    diagnostics::write(diagnostics::Level::info,"settings.autostart",enabled?"enabled":"disabled");
    const auto path = shortcut_path();
    const auto link = shell_link();
    if (std::filesystem::exists(path) && !owned(link.Get(), path))
        throw std::runtime_error("FluxTop.lnk already exists and is not a FluxTop-managed shortcut");
    if (!enabled) {
        if (std::filesystem::exists(path) && !DeleteFileW(path.c_str())) throw windows_error("Remove FluxTop startup shortcut");
        return;
    }
    const auto script = bootstrap();
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) throw windows_error("GetSystemDirectoryW");
    const bool portable=script.extension()==L".exe";
    const auto host = portable ? script : std::filesystem::path(system) / L"wscript.exe";
    const auto arguments = portable ? std::wstring(L"--autostart") : L"//B //Nologo \"" + script.wstring() + L"\"";
    if (FAILED(link->SetPath(host.c_str())) || FAILED(link->SetArguments(arguments.c_str()))
        || FAILED(link->SetWorkingDirectory((portable ? script.parent_path() : script.parent_path().parent_path()).c_str()))
        || FAILED(link->SetDescription(description)) || FAILED(link->SetShowCmd(SW_SHOWNORMAL)))
        throw std::runtime_error("Cannot configure the FluxTop startup shortcut");
    ComPtr<IPersistFile> file;
    const auto temporary = path.parent_path() / L"FluxTop.pending.lnk";
    if (FAILED(link.As(&file)) || FAILED(file->Save(temporary.c_str(), TRUE)))
        throw std::runtime_error("Cannot save the FluxTop startup shortcut");
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(code);
        throw windows_error("Publish FluxTop startup shortcut");
    }
    // Explicit re-enabling from the client resets only this app's Windows approval entry.
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, approved_key, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"FluxTop.lnk"); RegCloseKey(key);
    }
}
} // namespace fluxtop
