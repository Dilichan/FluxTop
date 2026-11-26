#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fluxtop::detail {

// QPC timestamps and process identity are validated by the foreground ETW adapter.
// Separate banks preserve different swap chains. A single active chain is required
// when no event connects the swap-chain address to the foreground HWND.
class PresentedFrameWindow {
public:
    enum class Runtime : std::uint8_t { dxgi, dxgi_mpo, d3d9 };
    explicit PresentedFrameWindow(std::int64_t frequency) noexcept;
    void reset(std::int64_t boundary) noexcept;
    void discard(std::int64_t boundary) noexcept;
    void start(std::uint32_t thread, Runtime runtime, std::uint64_t chain,
               std::int64_t ticks, bool ignored) noexcept;
    void stop(std::uint32_t thread, Runtime runtime, std::int64_t ticks, bool successful) noexcept;
    std::optional<double> rate(std::int64_t now) noexcept;
    std::uint64_t invalid_intervals() const noexcept { return invalid_intervals_; }

private:
    struct Pending {
        std::uint32_t thread{};
        Runtime runtime{};
        std::uint64_t chain{};
        std::int64_t begin{};
        bool ignored{};
    };
    struct Bin {
        std::int64_t id{-1};
        std::int64_t first{};
        std::int64_t last{};
        std::uint32_t frames{};
    };
    struct Chain {
        std::uint64_t address{};
        bool d3d9{};
        std::int64_t last{};
        std::array<Bin, 102> bins{};
    };
    void add(const Pending& pending, std::int64_t end) noexcept;
    Pending* find_pending(std::uint32_t thread, Runtime runtime) noexcept;
    void remove_pending(Pending* pending) noexcept;
    const std::int64_t frequency_;
    const std::int64_t bin_width_;
    std::int64_t boundary_{};
    std::uint64_t invalid_intervals_{};
    std::array<Pending, 32> pending_{};
    std::size_t pending_count_{};
    std::array<Chain, 12> chains_{};
};

} // namespace fluxtop::detail
