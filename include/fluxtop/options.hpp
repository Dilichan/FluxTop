#pragma once

#include "fluxtop/model.hpp"
#include "fluxtop/collection_plan.hpp"
#include <span>
#include <string_view>

namespace fluxtop {
struct Options {
    unsigned interval_ms = 1000;
    unsigned samples = 0;
    unsigned top = 10;
    unsigned font_size = 16;
    Sort sort = Sort::cpu;
    CollectionPlan collectors;
    bool jsonl = false;
    bool help = false;
    bool version = false;
    bool overlay = false;
    bool tray = false;
    bool top_explicit = false;
    bool sort_explicit = false;
    bool font_explicit = false;
    bool interval_explicit = false;
    constexpr CollectionPlan collection_plan() const {
        auto result=collectors;
        result.fps=overlay;
        return result;
    }
};
Options parse_options(std::span<const std::string_view> args);
std::string_view usage();
} // namespace fluxtop
