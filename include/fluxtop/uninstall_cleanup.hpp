#pragma once
#include <filesystem>

namespace fluxtop {
// Removes only FluxTop executable components from an already validated installation.
// Does not touch user databases, settings, logs, or unknown files.
void remove_installed_binaries(const std::filesystem::path& install_root);
}
