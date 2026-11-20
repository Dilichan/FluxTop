#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace fluxtop {
// Returns true when the currently logged-in session already owns a FluxTop overlay window.
bool monitor_running() noexcept;
// Extracts version metadata from a trusted local FluxTop launcher resource.
std::wstring launcher_release_tag(const std::filesystem::path& launcher);
// True when both paths refer to the same on-disk file.
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) noexcept;
// True when the current-user Windows installed-app registration belongs to FluxTop.
bool application_registered(const std::filesystem::path& install_path);
// Checks whether the Installed Apps DisplayVersion matches the active launcher.
bool installation_version_current(std::wstring_view tag);
// Installs/upgrades a single-file launcher without changing history or Startup settings.
void install_application(const std::filesystem::path& source,
    const std::filesystem::path& data_directory, std::wstring_view tag, bool register_windows);
// Starts the installed launcher (not the embedded monitor). Does not bypass UAC.
void launch_installed_application(const std::filesystem::path& data_directory, bool tray);
// Delegates removal to the installed native EXE; it confirms, waits and preserves user data.
void uninstall_application(const std::filesystem::path& data_directory);
} // namespace fluxtop
