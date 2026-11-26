#pragma once

#include <windows.h>
#include <evntrace.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fluxtop {

enum class EtwSessionKind { network, disk_cpu, presentation };

struct EtwSessionOwner {
    DWORD pid;
    std::uint64_t creation_time;
};
std::optional<EtwSessionOwner> parse_etw_session_owner(EtwSessionKind kind,std::wstring_view name) noexcept;

struct EtwSessionConfig {
    ULONG events;
    ULONG buffer_kib;
    ULONG minimum_buffers;
    ULONG maximum_buffers;
    bool per_processor_buffers;
};
struct EtwSessionHealth {
    ULONG status;
    ULONG events_lost;
    ULONG realtime_buffers_lost;
    ULONG log_buffers_lost;
    bool complete() const noexcept {
        return status==ERROR_SUCCESS && !events_lost && !realtime_buffers_lost && !log_buffers_lost;
    }
};

// Owns the tracing session. Consumers choose their timestamp format and callbacks.
class EtwSession {
public:
    EtwSession()=default;
    ~EtwSession();
    EtwSession(const EtwSession&)=delete;
    EtwSession& operator=(const EtwSession&)=delete;
    ULONG start(EtwSessionKind kind,const EtwSessionConfig& config);
    ULONG enable_provider(const GUID& provider, UCHAR level, ULONGLONG keywords,
                          ENABLE_TRACE_PARAMETERS parameters) const noexcept;
    void stop() noexcept;
    EtwSessionHealth health() const noexcept;
    bool active() const noexcept {return handle_!=0;}
    wchar_t* name() noexcept {return storage_.name;} // Win32's LoggerName is a mutable pointer.
private:
    struct Properties {
        EVENT_TRACE_PROPERTIES data{};
        wchar_t name[96]{};
    } storage_;
    TRACEHANDLE handle_{};
};
} // namespace fluxtop
