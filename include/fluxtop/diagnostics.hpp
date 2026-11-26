#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

// FluxTop-wide, non-blocking application event journal.
// Each event is bounded and is written on a dedicated thread. The five
// on-disk segments have a combined hard budget of 500 MiB.
namespace fluxtop::diagnostics {
enum class Level : std::uint8_t { trace, info, warning, error };
void write(Level level, std::string_view category, std::string_view message) noexcept;
void flush() noexcept;
std::filesystem::path log_directory() noexcept;
}
