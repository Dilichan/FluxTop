#pragma once

#include "fluxtop/model.hpp"
#include <span>
#include <string_view>

namespace fluxtop {
struct Options {
    unsigned interval_ms = 1000;
    unsigned samples = 0;
    unsigned top = 10;
    Sort sort = Sort::cpu;
    bool gpu = true;
    bool jsonl = false;
    bool help = false;
    bool version = false;
};
Options parse_options(std::span<const std::string_view> args);
std::string_view usage();
} // namespace fluxtop
