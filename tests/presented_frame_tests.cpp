#include "fluxtop/presented_frame_window.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using Window = fluxtop::detail::PresentedFrameWindow;
constexpr std::int64_t second = 100'000;
constexpr std::int64_t period = 1'250; // 80 presents per second.

void need(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

void frame(Window& window, std::uint64_t chain, std::int64_t begin, std::uint32_t thread = 7,
           Window::Runtime api = Window::Runtime::dxgi, bool ignored = false, bool successful = true) {
    window.start(thread, api, chain, begin, ignored);
    window.stop(thread, api, begin + 7, successful);
}

void frames(Window& window, std::uint64_t chain, std::int64_t begin, int count,
            Window::Runtime api = Window::Runtime::dxgi) {
    for (int i = 0; i < count; ++i) frame(window, chain, begin + period * i, 7, api);
}

void expect_rate(Window& window, std::int64_t now, double expected) {
    const auto rate = window.rate(now);
    need(rate && std::abs(*rate - expected) < 0.0001, "measured QPC presentation rate");
}

void test_timing() {
    Window window(second);
    window.reset(second);
    for (int i = 0; i < 1000; ++i) {
        const auto begin = second + i * period;
        frame(window, 17, begin, static_cast<std::uint32_t>(i % 3 + 1));
        if (i > 100) expect_rate(window, begin + 7, 80.0);
    }
    need(!window.rate(second * 16), "an old frame cannot keep an FPS reading alive");
    frames(window, 17, second * 16, 100);
    expect_rate(window, second * 17 + 24 * period, 80.0);
}

void test_focus_and_loss() {
    Window window(second);
    window.reset(second);
    frames(window, 11, second, 90);
    expect_rate(window, 2 * second + 10 * period, 80.0);

    window.reset(3 * second);
    frames(window, 11, second, 150); // A delayed batch predates the new foreground epoch.
    need(!window.rate(4 * second), "a foreground change invalidates buffered old frames");
    frames(window, 11, 3 * second, 90);
    expect_rate(window, 4 * second + 10 * period, 80.0);

    window.discard(5 * second);
    need(!window.rate(5 * second + period), "ETW loss invalidates the entire measurement window");
    frames(window, 11, 5 * second, 90);
    expect_rate(window, 6 * second + 10 * period, 80.0);
}

void test_attribution() {
    Window window(second);
    window.reset(second);
    for (int i = 0; i < 120; ++i) {
        const auto begin = second + i * period;
        frame(window, 11, begin);
        frame(window, 12, begin + 10);
    }
    need(!window.rate(3 * second), "independent swap chains are never added together");
    frames(window, 11, 3 * second, 180);
    expect_rate(window, 5 * second + 25 * period, 80.0);

    window.reset(7 * second);
    for (int i = 0; i < 120; ++i) {
        const auto begin = 7 * second + i * period;
        frame(window, 11, begin, 7, Window::Runtime::d3d9);
        frame(window, 12, begin + 10, 7, Window::Runtime::dxgi, true);
        frame(window, 13, begin + 20, 7, Window::Runtime::dxgi, false, false);
    }
    expect_rate(window, 8 * second + 40 * period, 80.0);
}

void test_partial_and_bounds() {
    Window window(second);
    window.reset(second);
    frames(window, 11, second, 100);
    expect_rate(window, 2 * second + 20 * period, 80.0);
    window.stop(90, Window::Runtime::dxgi, 3 * second, true);
    need(!window.rate(3 * second + period) && window.invalid_intervals() == 1,
        "an unmatched completion invalidates a running trace");
    frames(window, 11, 3 * second, 90);
    expect_rate(window, 4 * second + 10 * period, 80.0);

    window.reset(5 * second);
    for (std::uint32_t i = 0; i < 34; ++i)
        window.start(i + 1, Window::Runtime::dxgi, 11, 5 * second + i, false);
    need(!window.rate(6 * second) && window.invalid_intervals() == 2,
        "in-flight entries remain bounded even with incomplete records");
    window.reset(7 * second);
    for (int i = 0; i < 15; ++i)
        frame(window, static_cast<std::uint64_t>(i + 1), 7 * second + period * i);
    need(!window.rate(8 * second) && window.invalid_intervals() == 3,
        "the swap-chain cache is bounded and reports ambiguity");
}
} // namespace

int main() {
    try {
        test_timing();
        test_focus_and_loss();
        test_attribution();
        test_partial_and_bounds();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
