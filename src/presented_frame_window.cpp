#include "fluxtop/presented_frame_window.hpp"

#include <algorithm>

namespace fluxtop::detail {

PresentedFrameWindow::PresentedFrameWindow(std::int64_t frequency) noexcept
    : frequency_(frequency), bin_width_(std::max<std::int64_t>(1, frequency / 100)) {}

void PresentedFrameWindow::reset(std::int64_t boundary) noexcept {
    boundary_ = boundary;
    pending_count_ = 0;
    for (auto& chain : chains_) chain.address = 0;
}

void PresentedFrameWindow::discard(std::int64_t boundary) noexcept {
    ++invalid_intervals_;
    reset(std::max(boundary_, boundary));
}

PresentedFrameWindow::Pending* PresentedFrameWindow::find_pending(
    std::uint32_t thread, Runtime runtime) noexcept {
    const auto end = pending_.begin() + pending_count_;
    const auto it = std::find_if(pending_.begin(), end, [=](const Pending& pending) {
        return pending.thread == thread && pending.runtime == runtime;
    });
    return it == end ? nullptr : &*it;
}

void PresentedFrameWindow::remove_pending(Pending* pending) noexcept {
    *pending = pending_[--pending_count_];
}

void PresentedFrameWindow::start(std::uint32_t thread, Runtime runtime, std::uint64_t chain,
                                  std::int64_t ticks, bool ignored) noexcept {
    if (ticks < boundary_) return;
    if (!chain || pending_count_ == pending_.size() || find_pending(thread, runtime)) {
        discard(ticks);
        return;
    }
    pending_[pending_count_++] = {thread, runtime, chain, ticks, ignored};
}

void PresentedFrameWindow::stop(std::uint32_t thread, Runtime runtime, std::int64_t ticks,
                                 bool successful) noexcept {
    if (ticks < boundary_) return;
    auto* found = find_pending(thread, runtime);
    if (!found) {
        // The consumer can attach while a Present is already in progress.
        if (ticks - boundary_ >= frequency_) discard(ticks);
        return;
    }
    const Pending pending = *found;
    remove_pending(found);
    if (ticks < pending.begin || ticks - pending.begin > frequency_ * 2) {
        discard(ticks);
        return;
    }
    if (successful && !pending.ignored) add(pending, ticks);
}

void PresentedFrameWindow::add(const Pending& pending, std::int64_t end) noexcept {
    const bool d3d9 = pending.runtime == Runtime::d3d9;
    Chain* active = nullptr;
    Chain* unused = nullptr;
    for (auto& chain : chains_) {
        if (!chain.address || end - chain.last > frequency_ * 2) {
            if (!unused) unused = &chain;
        } else if (chain.address == pending.chain && chain.d3d9 == d3d9) {
            active = &chain;
            break;
        }
    }
    if (!active) {
        if (!unused) {
            discard(end);
            return;
        }
        *unused = {};
        unused->address = pending.chain;
        unused->d3d9 = d3d9;
        active = unused;
    } else if (pending.begin <= active->last) {
        discard(end);
        return;
    }
    if (pending.begin - active->last > frequency_ * 2) {
        // A resumed or recreated swap chain must warm up with new frames.
        for (auto& bin : active->bins) bin.id = -1;
    }
    active->last = pending.begin;
    const auto id = pending.begin / bin_width_;
    auto& bin = active->bins[static_cast<std::size_t>(id) % active->bins.size()];
    if (bin.id != id) bin = {id, pending.begin, pending.begin, 0};
    bin.last = pending.begin;
    ++bin.frames;
}

std::optional<double> PresentedFrameWindow::rate(std::int64_t now) noexcept {
    if (frequency_ <= 0 || now - boundary_ < frequency_) return std::nullopt;
    for (std::size_t i = 0; i < pending_count_; ++i) {
        if (now - pending_[i].begin > frequency_ * 2) {
            discard(now);
            return std::nullopt;
        }
    }
    const Chain* active = nullptr;
    for (auto& chain : chains_) {
        if (!chain.address) continue;
        if (now - chain.last > frequency_ * 2) {
            chain.address = 0;
            continue;
        }
        if (chain.last > now || active) return std::nullopt;
        active = &chain;
    }
    if (!active || now - active->last > frequency_ * 3 / 2) return std::nullopt;

    const auto earliest = active->last - frequency_;
    std::int64_t first = active->last;
    std::uint64_t frames = 0;
    for (const auto& bin : active->bins) {
        if (bin.id < 0 || bin.first < earliest) continue;
        frames += bin.frames;
        first = std::min(first, bin.first);
    }
    const auto span = active->last - first;
    if (frames < 3 || span < frequency_ / 2) return std::nullopt;
    const auto fps = static_cast<double>(frames - 1) * static_cast<double>(frequency_) /
                     static_cast<double>(span);
    return fps <= 10000.0 ? std::optional{fps} : std::nullopt;
}

} // namespace fluxtop::detail
