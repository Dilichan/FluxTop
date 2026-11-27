#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <windows.h>

namespace fluxtop {
struct AppIcon {
    static constexpr unsigned size = 32;
    std::array<std::byte, size * size * 4> pixels{}; // Premultiplied BGRA.
};
struct IconCacheStats {
    std::size_t entries{}, pending{}, capacity{};
    std::uint64_t disk_hits{}, extractions{};
    bool persistent{};
};
// Optional diagnostic export of an owned render surface; no desktop capture.
bool save_argb_png(const std::filesystem::path& file, const void* pixels, unsigned width, unsigned height);
// Converts cached pixels for native Win32 controls. The caller owns the handle.
HICON native_app_icon(const AppIcon& image);
class AppIconCache {
public:
    AppIconCache(std::filesystem::path database, std::function<void()> ready);
    ~AppIconCache();
    AppIconCache(const AppIconCache&) = delete;
    AppIconCache& operator=(const AppIconCache&) = delete;
    // Configures the bounded working set to accommodate visible rows and churn.
    void set_capacity(std::size_t capacity);
    // All views use the same canonical path provided by the process snapshot or history.
    void request(std::wstring_view exe_path);
    std::shared_ptr<const AppIcon> get(std::wstring_view exe_path);
    IconCacheStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fluxtop
