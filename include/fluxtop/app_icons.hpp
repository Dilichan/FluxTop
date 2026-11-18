#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace fluxtop {
struct AppIcon {
    static constexpr unsigned size = 32;
    std::array<std::byte, size * size * 4> pixels{}; // Premultiplied BGRA.
};
struct IconCacheStats {
    std::size_t entries{}, pending{};
    std::uint64_t disk_hits{}, extractions{};
    bool persistent{};
};
// Optional diagnostic export of an owned render surface; no desktop capture.
bool save_argb_png(const std::filesystem::path& file, const void* pixels, unsigned width, unsigned height);
class AppIconCache {
public:
    AppIconCache(std::filesystem::path database, std::function<void()> ready);
    ~AppIconCache();
    AppIconCache(const AppIconCache&) = delete;
    AppIconCache& operator=(const AppIconCache&) = delete;
    void request(std::uint32_t pid, std::optional<std::uint64_t> created);
    std::shared_ptr<const AppIcon> get(std::uint32_t pid, std::optional<std::uint64_t> created);
    IconCacheStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fluxtop
