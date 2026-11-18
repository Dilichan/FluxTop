#pragma once
#include "fluxtop/model.hpp"
#include <filesystem>
#include <memory>
namespace fluxtop {
std::filesystem::path history_path();
class HistoryWriter {
public:
    explicit HistoryWriter(std::filesystem::path path);
    ~HistoryWriter();
    void append(Snapshot& snapshot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
