#pragma once
#include <filesystem>
namespace fluxtop {
// True when the installed PawnIO device accepts user-mode access.
bool pawnio_device_ready() noexcept;
// For safe, offline package tests. Never runs the bundled driver installer.
bool pawnio_bundle_verified(const std::filesystem::path& launcher) noexcept;
// Called only after the user agreed to install/update FluxTop via UAC.
void maybe_install_embedded_pawnio(const std::filesystem::path& launcher,
                                  const std::filesystem::path& data_directory) noexcept;
}