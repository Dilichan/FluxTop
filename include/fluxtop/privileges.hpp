#pragma once
#include <optional>

namespace fluxtop {
struct Privileges {
    std::optional<bool> elevated;
    bool debug_enabled{};
};
Privileges initialize_privileges();
} // namespace fluxtop
