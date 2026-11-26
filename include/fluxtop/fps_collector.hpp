#pragma once
#include "fluxtop/model.hpp"
#include <memory>

namespace fluxtop {
class FpsCollector {
public:
    FpsCollector();
    ~FpsCollector();
    FpsCollector(const FpsCollector&) = delete;
    FpsCollector& operator=(const FpsCollector&) = delete;
    void collect(std::vector<Process>& processes, FpsFocus focus) noexcept;
private:
    class Trace;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace fluxtop
