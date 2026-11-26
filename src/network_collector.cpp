#include "fluxtop/network_collector.hpp"
#include "fluxtop/etw_session.hpp"
#include "fluxtop/diagnostics.hpp"
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace fluxtop {
namespace {
constexpr GUID tcp{0x9a280ac0,0xc8e0,0x11d1,{0x84,0xe2,0x00,0xc0,0x4f,0xb9,0x98,0xa2}};
constexpr GUID udp{0xbf3a50c5,0xa9c9,0x4988,{0xa0,0x05,0x2d,0xf0,0xb7,0xc8,0x0f,0x80}};
struct Traffic { std::uint64_t rx{}, tx{}, first_event{}; };
}
struct NetworkCollector::Impl {
    EtwSession session;
    TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread;
    std::mutex mutex;
    std::unordered_map<std::uint32_t, Traffic> traffic;
    Traffic total;
    std::atomic<bool> failed{false};
    bool baseline = false;
    std::string status = "unavailable";
    std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();

    static void WINAPI event(EVENT_RECORD* record) noexcept {
        auto& self = *static_cast<Impl*>(record->UserContext);
        const auto& id = record->EventHeader.ProviderId;
        if (!IsEqualGUID(id, tcp) && !IsEqualGUID(id, udp)) return;
        const auto opcode = record->EventHeader.EventDescriptor.Opcode;
        const bool send = opcode == 10 || opcode == 26;
        if (!send && opcode != 11 && opcode != 27) return;
        try {
            DWORD pid{}, size{};
            const auto property = [&](const wchar_t* name, DWORD& value) {
                PROPERTY_DATA_DESCRIPTOR descriptor{};
                descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
                descriptor.ArrayIndex = ULONG_MAX;
                return TdhGetProperty(record, 0, nullptr, 1, &descriptor, sizeof(value), reinterpret_cast<BYTE*>(&value)) == ERROR_SUCCESS;
            };
            if (!property(L"PID", pid) || !property(L"size", size)) { self.failed = true; return; }
            std::lock_guard lock(self.mutex);
            // Bounded transient map, emptied at every sample; never a persistent packet log.
            if (!self.traffic.contains(pid) && self.traffic.size() >= 65536) { self.failed = true; return; }
            auto& row = self.traffic[pid];
            const auto stamp = static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart);
            if (!row.first_event || stamp < row.first_event) row.first_event = stamp;
            if (send) { row.tx += size; self.total.tx += size; }
            else { row.rx += size; self.total.rx += size; }
        } catch (...) { self.failed = true; }
    }
    static ULONG WINAPI buffer(EVENT_TRACE_LOGFILEW* log) noexcept {
        if (log->EventsLost) static_cast<Impl*>(log->Context)->failed = true;
        return TRUE;
    }
    explicit Impl(bool enabled) {
        if (!enabled) { status = "disabled"; return; }
        const auto result=session.start(EtwSessionKind::network,{EVENT_TRACE_FLAG_NETWORK_TCPIP,64,2,8,false});
        if (result != ERROR_SUCCESS) {
            status = result == ERROR_ACCESS_DENIED ? "administrator-required" : "unavailable"; return;
        }
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = session.name();
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback = event; log.BufferCallback = buffer; log.Context = this;
        consumer = OpenTraceW(&log);
        if (consumer == INVALID_PROCESSTRACE_HANDLE) { stop(); return; }
        try {
            thread = std::thread([this] {
                const auto result = ProcessTrace(&consumer, 1, nullptr, nullptr);
                if (result != ERROR_SUCCESS && result != ERROR_CANCELLED) failed = true;
            });
            status = "ready";
        } catch (...) { stop(); throw; }
    }
    void stop() noexcept {
        session.stop();
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(consumer);
            if (thread.joinable()) thread.join();
            consumer = INVALID_PROCESSTRACE_HANDLE;
        }
    }
    ~Impl() { stop(); }
};
NetworkCollector::NetworkCollector(bool enabled) : impl_(std::make_unique<Impl>(enabled)),enabled_(enabled) {}
NetworkCollector::~NetworkCollector() = default;
void NetworkCollector::collect(Snapshot& snapshot) {
    const auto recovery_now=EtwRecovery::Clock::now();
    if(enabled_ && recovery_.due(recovery_now)){
        impl_.reset();impl_=std::make_unique<Impl>(true);recovery_.restarted(recovery_now);
        diagnostics::write(diagnostics::Level::info,"network.recovery","restarted owned network session");
    }
    auto& self = *impl_;
    snapshot.total_rx_bytes_per_second.reset();snapshot.total_tx_bytes_per_second.reset();
    for(auto& row:snapshot.processes){row.rx_bytes_per_second.reset();row.tx_bytes_per_second.reset();}
    if(self.session.active() && !self.session.health().complete())self.failed=true;
    if(enabled_ && (self.failed || self.status=="unavailable")){
        if(recovery_.schedule(recovery_now))diagnostics::write(diagnostics::Level::warning,"network.recovery","scheduled fresh trace after failure");
    }else recovery_.healthy(recovery_now);
    std::unordered_map<std::uint32_t, Traffic> traffic;
    Traffic total;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - self.previous).count();
    self.previous = now;
    {
        std::lock_guard lock(self.mutex);
        traffic.swap(self.traffic); total = self.total; self.total = {};
    }
    snapshot.network_status = self.failed ? "incomplete" : self.status;
    if (self.status != "ready" || self.failed || seconds <= 0) return;
    if (!self.baseline) { self.baseline = true; return; }
    snapshot.total_rx_bytes_per_second = static_cast<double>(total.rx) / seconds;
    snapshot.total_tx_bytes_per_second = static_cast<double>(total.tx) / seconds;
    for (auto& row : snapshot.processes) {
        if (!row.has_baseline || !row.creation_time) continue;
        const auto found = traffic.find(row.pid);
        if (found != traffic.end() && found->second.first_event < *row.creation_time) continue;
        row.rx_bytes_per_second = found == traffic.end() ? 0.0 : static_cast<double>(found->second.rx) / seconds;
        row.tx_bytes_per_second = found == traffic.end() ? 0.0 : static_cast<double>(found->second.tx) / seconds;
    }
}
} // namespace fluxtop
