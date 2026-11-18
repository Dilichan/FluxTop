#pragma once
#include "fluxtop/model.hpp"
#include <iosfwd>
#include <string_view>

namespace fluxtop {
std::string json_escape(std::string_view input);
void write_jsonl(std::ostream& out, const Snapshot& snapshot, unsigned top);
void write_table(std::ostream& out, const Snapshot& snapshot, unsigned top);
} // namespace fluxtop
