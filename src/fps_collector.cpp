#include "fluxtop/fps_collector.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/etw_recovery.hpp"
#include "fluxtop/etw_session.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/presented_frame_window.hpp"

#include <evntcons.h>
#include <dxgi.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace fluxtop {
namespace {

using Window = detail::PresentedFrameWindow;
constexpr GUID dxgi_provider{0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}};
constexpr GUID d3d9_provider{0x783aca0a, 0x790e, 0x4d7f, {0x84, 0x51, 0xaa, 0x85, 0x05, 0x11, 0xc6, 0xb9}};
constexpr ULONG64 present_keyword = 0x8000000000000002ULL;
constexpr USHORT dxgi_start = 42, dxgi_stop = 43, mpo_start = 55, mpo_stop = 56;
constexpr USHORT d3d9_start = 1, d3d9_stop = 2;
constexpr std::size_t max_targets = 4;
constexpr ULONG trace_running = std::numeric_limits<ULONG>::max();

std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t qpc_frequency() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

struct SourceIdentity {
    DWORD pid{};
    std::uint64_t creation{};
    HWND window{};
    bool operator==(const SourceIdentity&) const = default;
};

struct Sources {
    std::array<SourceIdentity, max_targets> identities{};
    std::size_t count{};
    bool overflow{};
    bool operator==(const Sources&) const = default;

    void add(SourceIdentity identity) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (identities[i].pid == identity.pid) return;
        }
        if (count == identities.size()) {
            overflow = true;
            return;
        }
        identities[count++] = identity;
    }
};

const Process* lookup(const std::vector<Process>& processes, DWORD pid) noexcept {
    const auto it = std::find_if(processes.begin(), processes.end(), [pid](const Process& row) {
        return row.pid == pid;
    });
    return it == processes.end() ? nullptr : &*it;
}

std::uint64_t creation_time(const std::vector<Process>& processes, DWORD pid) noexcept {
    if (!pid || pid == GetCurrentProcessId()) return 0;
    const auto* row = lookup(processes, pid);
    return row && row->has_baseline ? row->creation_time.value_or(0) : 0;
}

bool current_foreground(HWND hwnd, DWORD pid) noexcept {
    if (!hwnd || GetForegroundWindow() != hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD owner{};
    GetWindowThreadProcessId(hwnd, &owner);
    return owner && owner == pid;
}

Sources find_sources(const std::vector<Process>& processes, HWND foreground, SourceIdentity primary) noexcept {
    Sources sources;
    sources.add(primary);
    struct Enumeration {
        const std::vector<Process>& processes;
        Sources& sources;
        unsigned visited{};
    } ctx{processes, sources};
    // A different PID is eligible only if it owns a visible descendant of the focused
    // window. A process tree or shared image path alone cannot establish a render target.
    EnumChildWindows(foreground, [](HWND child, LPARAM data) -> BOOL {
        auto& e = *reinterpret_cast<Enumeration*>(data);
        if (++e.visited > 128) {
            e.sources.overflow = true;
            return FALSE;
        }
        if (!IsWindowVisible(child)) return TRUE;
        DWORD pid{};
        GetWindowThreadProcessId(child, &pid);
        if (pid == e.sources.identities[0].pid) return TRUE;
        if (const auto birth = creation_time(e.processes, pid)) e.sources.add({pid, birth, child});
        return !e.sources.overflow;
    }, reinterpret_cast<LPARAM>(&ctx));
    return sources;
}

bool sources_match(const Sources& sources, const std::vector<Process>& processes, HWND hwnd) noexcept {
    for (std::size_t i = 0; i < sources.count; ++i) {
        const auto& source = sources.identities[i];
        if (source.creation != creation_time(processes, source.pid)) return false;
        if (i == 0) continue;
        DWORD owner{};
        GetWindowThreadProcessId(source.window, &owner);
        if (owner != source.pid || !IsWindowVisible(source.window) || !IsChild(hwnd, source.window))
            return false;
    }
    return true;
}

template<typename T>
T payload(const EVENT_RECORD& event, std::size_t offset = 0) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(event.UserData) + offset, sizeof(value));
    return value;
}

struct EventFilter {
    BOOLEAN include{TRUE};
    UCHAR reserved{};
    USHORT count{};
    USHORT ids[4]{};
};
static_assert(offsetof(EventFilter, ids) == offsetof(EVENT_FILTER_EVENT_ID, Events));

ULONG enable_provider(const EtwSession& session, const GUID& provider, const EventFilter& ids,
                      const std::array<DWORD, max_targets>& pids, std::size_t count) noexcept {
    std::array<EVENT_FILTER_DESCRIPTOR, 2> filters{};
    filters[0] = {reinterpret_cast<ULONG64>(&ids),
        static_cast<ULONG>(offsetof(EventFilter, ids) + ids.count * sizeof(USHORT)), EVENT_FILTER_TYPE_EVENT_ID};
    filters[1] = {reinterpret_cast<ULONG64>(pids.data()), static_cast<ULONG>(count * sizeof(DWORD)),
        EVENT_FILTER_TYPE_PID};
    ENABLE_TRACE_PARAMETERS parameters{};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableFilterDesc = filters.data();
    parameters.FilterDescCount = static_cast<ULONG>(filters.size());
    return session.enable_provider(provider, TRACE_LEVEL_VERBOSE, present_keyword, parameters);
}

bool lost_data(const EtwSessionHealth& a, const EtwSessionHealth& b) noexcept {
    return a.events_lost != b.events_lost || a.realtime_buffers_lost != b.realtime_buffers_lost ||
           a.log_buffers_lost != b.log_buffers_lost;
}

} // namespace

class FpsCollector::Trace {
    struct Target {
        SourceIdentity identity;
        Window window;
    };
public:
    Trace() : frequency_(qpc_frequency()) { targets_.reserve(max_targets); }
    ~Trace() { close(); }

    void clear() noexcept {
        std::lock_guard lock(mutex_);
        targets_.clear();
    }

    ULONG start() {
        if (frequency_ <= 0) return ERROR_NOT_SUPPORTED;
        // A provider-only session uses no kernel logger slots required by CPU, disk or network.
        const auto status = session_.start(EtwSessionKind::presentation, {0, 8, 2, 8, false});
        if (status != ERROR_SUCCESS) return status;
        EVENT_TRACE_LOGFILEW logfile{};
        logfile.LoggerName = session_.name();
        logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                                   PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logfile.EventRecordCallback = &Trace::consume;
        logfile.Context = this;
        consumer_ = OpenTraceW(&logfile);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            const auto error = GetLastError();
            session_.stop();
            return error;
        }
        worker_ = std::thread([this] {
            TRACEHANDLE handle = consumer_;
            trace_status_.store(ProcessTrace(&handle, 1, nullptr, nullptr), std::memory_order_release);
        });
        return ERROR_SUCCESS;
    }

    ULONG select(const Sources& sources) {
        std::array<DWORD, max_targets> pids{};
        const auto boundary = qpc_now();
        {
            std::lock_guard lock(mutex_);
            targets_.clear();
            for (std::size_t i = 0; i < sources.count; ++i) {
                const auto& source = sources.identities[i];
                pids[i] = source.pid;
                targets_.push_back({source, Window(frequency_)});
                targets_.back().window.reset(boundary);
            }
        }
        auto status = enable_provider(session_, dxgi_provider,
            {TRUE, 0, 4, {dxgi_start, dxgi_stop, mpo_start, mpo_stop}}, pids, sources.count);
        if (status != ERROR_SUCCESS) return status;
        status = enable_provider(session_, d3d9_provider,
            {TRUE, 0, 2, {d3d9_start, d3d9_stop}}, pids, sources.count);
        return status;
    }

    bool active() const noexcept { return session_.active(); }

    void close() noexcept {
        session_.stop();
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE) CloseTrace(consumer_);
        if (worker_.joinable()) worker_.join();
        consumer_ = INVALID_PROCESSTRACE_HANDLE;
        trace_status_.store(trace_running, std::memory_order_relaxed);
        previous_health_ = {};
        clear();
    }

    struct Reading {
        std::optional<double> fps;
        DWORD pid{};
        ULONG error{};
        bool loss{};
    };
    Reading read() noexcept {
        const auto ended = trace_status_.load(std::memory_order_acquire);
        if (ended != trace_running) return {{}, 0, ended ? ended : ERROR_INVALID_HANDLE};
        const auto health = session_.health();
        if (health.status != ERROR_SUCCESS) return {{}, 0, health.status};
        const auto now = qpc_now();
        std::lock_guard lock(mutex_);
        if (lost_data(previous_health_, health)) {
            previous_health_ = health;
            for (auto& target : targets_) target.window.reset(now);
            return {{}, 0, ERROR_SUCCESS, true};
        }
        Reading reading{};
        for (auto& target : targets_) {
            if (const auto fps = target.window.rate(now)) {
                if (reading.fps) return {};
                reading.fps = fps;
                reading.pid = target.identity.pid;
            }
        }
        return reading;
    }

    std::uint64_t invalid_intervals() noexcept {
        std::lock_guard lock(mutex_);
        std::uint64_t total{};
        for (const auto& target : targets_) total += target.window.invalid_intervals();
        return total;
    }

private:
    static void WINAPI consume(EVENT_RECORD* event) noexcept {
        static_cast<Trace*>(event->UserContext)->consume_record(*event);
    }

    void consume_record(const EVENT_RECORD& event) noexcept {
        const auto& hdr = event.EventHeader;
        const auto id = hdr.EventDescriptor.Id;
        const bool dxgi = hdr.ProviderId == dxgi_provider;
        const bool d3d9 = hdr.ProviderId == d3d9_provider;
        if (!dxgi && !d3d9) return;

        const bool mpo = dxgi && (id == mpo_start || id == mpo_stop);
        const auto runtime = d3d9 ? Window::Runtime::d3d9 : mpo ? Window::Runtime::dxgi_mpo : Window::Runtime::dxgi;
        const bool start = id == (d3d9 ? d3d9_start : mpo ? mpo_start : dxgi_start);
        const bool stop = id == (d3d9 ? d3d9_stop : mpo ? mpo_stop : dxgi_stop);
        if (!start && !stop) return;

        const auto ticks = hdr.TimeStamp.QuadPart;
        std::lock_guard lock(mutex_);
        const auto target = std::find_if(targets_.begin(), targets_.end(), [&hdr](const Target& value) {
            return value.identity.pid == hdr.ProcessId;
        });
        if (target == targets_.end()) return;

        // The version-0 runtime events carry the swap-chain pointer and flags
        // on entry, and HRESULT on return. Reject unfamiliar descriptors.
        const auto size = hdr.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER ? sizeof(std::uint32_t) : sizeof(std::uint64_t);
        if (hdr.EventDescriptor.Version != 0 || !event.UserData ||
            event.UserDataLength < (start ? size + sizeof(std::uint32_t) : sizeof(HRESULT))) {
            target->window.discard(ticks);
            return;
        }
        if (start) {
            const auto chain = size == sizeof(std::uint32_t) ?
                payload<std::uint32_t>(event) : payload<std::uint64_t>(event);
            const auto flags = payload<std::uint32_t>(event, size);
            target->window.start(hdr.ThreadId, runtime, chain, ticks,
                (flags & (dxgi ? DXGI_PRESENT_TEST : D3DPRESENT_DONOTFLIP)) != 0);
        } else {
            target->window.stop(hdr.ThreadId, runtime, ticks, payload<HRESULT>(event) == S_OK);
        }
    }

    std::int64_t frequency_;
    EtwSession session_;
    TRACEHANDLE consumer_{INVALID_PROCESSTRACE_HANDLE};
    std::thread worker_;
    std::atomic<ULONG> trace_status_{trace_running};
    EtwSessionHealth previous_health_{};
    std::mutex mutex_;
    std::vector<Target> targets_;
};

struct FpsCollector::State {
    Trace trace;
    EtwRecovery recovery;
    Sources sources{};
    std::uint64_t epoch{};
    ULONGLONG next_discovery{};
    ULONGLONG next_diagnostic{};
    bool failed{};
    bool was_ready{};

    void unavailable(ULONG error, EtwRecovery::Clock::time_point now) {
        trace.close();
        sources = {};
        failed = true;
        if (recovery.schedule(now))
            diagnostics::write(diagnostics::Level::warning, "fps.trace", "retry win32=" + std::to_string(error));
    }
};

FpsCollector::FpsCollector() = default;
FpsCollector::~FpsCollector() = default;

void FpsCollector::collect(std::vector<Process>& processes, FpsFocus focus) noexcept {
    for (auto& row : processes) row.presented_fps.reset();
    const auto hwnd = reinterpret_cast<HWND>(focus.window);
    const auto birth = creation_time(processes, focus.pid);
    if (!birth || !current_foreground(hwnd, focus.pid)) {
        if (state_ && state_->trace.active()) {
            state_->trace.close();
            state_->sources = {};
        }
        return;
    }
    try {
        if (!state_) state_ = std::make_unique<State>();
        auto& state = *state_;
        const auto now = EtwRecovery::Clock::now();
        if (state.failed) {
            if (!state.recovery.due(now)) return;
            state.recovery.restarted(now);
            state.failed = false;
        }

        const auto tick = GetTickCount64();
        const SourceIdentity primary{focus.pid, birth, hwnd};
        if (!state.sources.count || state.epoch != focus.epoch || state.sources.identities[0] != primary ||
            tick >= state.next_discovery) {
            state.next_discovery = tick + 5000;
            auto sources = find_sources(processes, hwnd, primary);
            if (sources.overflow) {
                state.trace.close();
                state.sources = {};
                return;
            }
            if (sources != state.sources || state.epoch != focus.epoch) {
                if (!state.trace.active()) {
                    const auto status = state.trace.start();
                    if (status != ERROR_SUCCESS) {
                        state.unavailable(status, now);
                        return;
                    }
                    diagnostics::write(diagnostics::Level::info, "fps.trace", "started DXGI/D3D9 ETW");
                }
                if (const auto status = state.trace.select(sources); status != ERROR_SUCCESS) {
                    state.unavailable(status, now);
                    return;
                }
                state.sources = sources;
                state.epoch = focus.epoch;
                state.was_ready = false;
                state.next_diagnostic = 0;
            }
        }
        if (!sources_match(state.sources, processes, hwnd)) {
            state.trace.clear();
            state.sources = {};
            return;
        }

        const auto reading = state.trace.read();
        if (reading.error) {
            state.unavailable(reading.error, now);
            return;
        }
        if (reading.loss) {
            diagnostics::write(diagnostics::Level::warning, "fps.trace", "ETW loss; waiting for fresh frames");
            return;
        }
        state.recovery.healthy(now);
        if (!current_foreground(hwnd, focus.pid)) return;
        const auto row = std::find_if(processes.begin(), processes.end(), [focus](const Process& p) {
            return p.pid == focus.pid;
        });
        if (row != processes.end()) row->presented_fps = reading.fps;

        if (tick >= state.next_diagnostic || (reading.fps && !state.was_ready)) {
            state.next_diagnostic = tick + 30000;
            const auto result = reading.fps ? std::to_string(*reading.fps) : std::string("null");
            diagnostics::write(diagnostics::Level::info, "fps.sample",
                "foreground_pid=" + std::to_string(focus.pid) + " render_pid=" + std::to_string(reading.pid) +
                " fps=" + result + " invalid_windows=" + std::to_string(state.trace.invalid_intervals()));
        }
        state.was_ready = reading.fps.has_value();
    } catch (...) {
        state_.reset();
        diagnostics::write(diagnostics::Level::warning, "fps.trace", "collector reset after exception");
    }
}

} // namespace fluxtop
