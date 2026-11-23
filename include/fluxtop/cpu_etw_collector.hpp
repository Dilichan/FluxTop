#pragma once
#include "fluxtop/model.hpp"
#include <memory>
namespace fluxtop {
// Raw-QPC consumer of the EXISTING disk kernel session; never creates/stops a session.
class CpuEtwCollector {
public:
    explicit CpuEtwCollector(const wchar_t* session_name);
    ~CpuEtwCollector();
    void collect(Snapshot& snapshot);
    void events_lost();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
