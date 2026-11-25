#include "fluxtop/fps_collector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <utility>

namespace fluxtop {
namespace {
// The path is registered by the installed PresentMon service. Do not load a random DLL from PATH.
std::wstring installed_presentmon_dll() noexcept {
    constexpr wchar_t key[] = L"SOFTWARE\\INTEL\\PresentMon\\Service";
    constexpr wchar_t value[] = L"sharedMiddlewarePath";
    DWORD bytes = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 65536) return {};
    try {
        std::wstring path(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, flags, nullptr, path.data(), &bytes) != ERROR_SUCCESS)
            return {};
        path.resize(wcsnlen_s(path.c_str(), path.size()));
        return path;
    } catch (...) { return {}; }
}
// Avoid MSVC C4191 unsafe function-pointer-cast warning under /W4 /WX.
template<typename Fn> Fn endpoint(HMODULE dll, const char* name) noexcept {
    const FARPROC proc = GetProcAddress(dll, name);
    static_assert(sizeof(Fn) == sizeof(proc));
    Fn result{};
    std::memcpy(&result, &proc, sizeof(result));
    return result;
}
} // namespace

FpsCollector::~FpsCollector() { disconnect(); }

void FpsCollector::disconnect() noexcept {
    if (session_ && stop_) for (const auto& [pid, created] : tracked_) {
        (void)created;
        stop_(session_, pid);
    }
    tracked_.clear();
    if (query_ && free_query_) free_query_(query_);
    query_ = nullptr;
    if (session_ && close_) close_(session_);
    session_ = nullptr;
    if (library_) FreeLibrary(library_);
    library_ = nullptr;
    open_ = nullptr; close_ = nullptr; start_ = nullptr; stop_ = nullptr;
    register_ = nullptr; free_query_ = nullptr; poll_ = nullptr;
    element_ = QueryElement{};
}

bool FpsCollector::connect() noexcept {
    if (session_ && query_) return true;
    const ULONGLONG tick = GetTickCount64();
    if (tick < next_connect_attempt_) return false;
    next_connect_attempt_ = tick + 30000;
    auto dll = installed_presentmon_dll();
    if (dll.empty()) {
        // Official optional SDK loader, installed with PresentMon Service.
        wchar_t program_files[32768]{};
        const DWORD length=GetEnvironmentVariableW(L"ProgramFiles",program_files,32768);
        if (length && length < 32768) {
            std::wstring candidate(program_files,length);
            candidate += L"\\Intel\\PresentMon\\SDK\\PresentMonAPI2Loader.dll";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                dll=std::move(candidate);
        }
    }
    if (dll.empty()) return false;
    library_ = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library_) return false;
    open_ = endpoint<OpenFn>(library_, "pmOpenSession");
    close_ = endpoint<CloseFn>(library_, "pmCloseSession");
    start_ = endpoint<StartFn>(library_, "pmStartTrackingProcess");
    stop_ = endpoint<StopFn>(library_, "pmStopTrackingProcess");
    register_ = endpoint<RegisterFn>(library_, "pmRegisterDynamicQuery");
    free_query_ = endpoint<FreeQueryFn>(library_, "pmFreeDynamicQuery");
    poll_ = endpoint<PollFn>(library_, "pmPollDynamicQuery");
    if (!open_ || !close_ || !start_ || !stop_ || !register_ || !free_query_ || !poll_ ||
        open_(&session_) != 0 || !session_) {
        disconnect(); return false;
    }
    if (register_(session_, &query_, &element_, 1, 1000.0, 0.0) != 0 || !query_ ||
        element_.size != sizeof(double) || element_.offset + element_.size > 512) {
        disconnect(); return false;
    }
    return true;
}

void FpsCollector::collect(std::vector<Process>& processes,
    const std::vector<std::uint32_t>& targets) noexcept {
    if (targets.empty()) {
        // No foreground target: stop tracking without polling PresentMon.
        if (session_ && stop_) for (const auto& [pid, created] : tracked_) {
            (void)created;
            stop_(session_, pid);
        }
        tracked_.clear(); retry_after_.clear();
        return;
    }
    try {
        std::unordered_map<std::uint32_t, Process*> processes_by_pid;
        processes_by_pid.reserve(processes.size());
        for (auto& row : processes) processes_by_pid.emplace(row.pid, &row);

        constexpr std::size_t max_tracked_pids = 1; // Only the foreground PID.
        std::unordered_map<std::uint32_t, Process*> desired;
        desired.reserve(std::min(targets.size(), max_tracked_pids));
        for (const auto pid : targets) {
            if (desired.size() >= max_tracked_pids) break;
            if (pid == GetCurrentProcessId()) continue;
            if (const auto it = processes_by_pid.find(pid);
                it != processes_by_pid.end() && it->second->creation_time && it->second->has_baseline)
                desired.emplace(pid, it->second);
        }
        if(desired.empty()) {
            if(session_) { disconnect(); next_connect_attempt_=0; }
            retry_after_.clear();
            return;
        }
        if(!connect())return;
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            const auto wanted = desired.find(it->first);
            if (wanted == desired.end() || it->second != wanted->second->creation_time) {
                if (session_ && stop_) stop_(session_, it->first);
                it = tracked_.erase(it);
            } else ++it;
        }
        for (auto it = retry_after_.begin(); it != retry_after_.end();) {
            if (desired.find(it->first) == desired.end()) it = retry_after_.erase(it);
            else ++it;
        }
        const auto tick = GetTickCount64();
        std::array<std::uint8_t, 4096> blob{};
        const auto stride = element_.offset + element_.size;
        if (!stride || stride * 8 > blob.size()) return;
        for (const auto pid : targets) {
            const auto it = desired.find(pid);
            if (it == desired.end()) continue;
            Process& row = *it->second;
            if (tracked_.find(pid) == tracked_.end()) {
                if (const auto failed = retry_after_.find(pid);
                    failed != retry_after_.end() && tick < failed->second) continue;
                const int status = start_(session_, pid);
                // PresentMon API: 7=ALREADY_TRACKING; 8=UNABLE_TO_CREATE_NSM (error).
                if (status != 0 && status != 7) {
                    retry_after_[pid] = tick + 15000;
                    continue;
                }
                tracked_.emplace(pid, row.creation_time);
                retry_after_.erase(pid);
                continue; // Need actual frames before reporting an FPS value.
            }
            std::uint32_t chains = 8;
            const int status = poll_(query_, pid, blob.data(), &chains);
            if (status != 0) {
                if (status == 4 || status == 12 || status == 13) {
                    disconnect();
                    next_connect_attempt_ = GetTickCount64() + 30000;
                    return;
                }
                if (status == 6) { // INVALID_PID: stop tracking a recycled PID.
                    stop_(session_, pid);
                    tracked_.erase(pid);
                    retry_after_[pid] = tick + 15000;
                }
                continue;
            }
            if (chains > 8 || stride * chains > blob.size()) continue;
            std::optional<double> best;
            for (std::uint32_t i = 0; i < chains; ++i) {
                double fps{};
                std::memcpy(&fps, blob.data() + i * stride + element_.offset, sizeof(fps));
                if (std::isfinite(fps) && fps > 0.0 && fps <= 10000.0)
                    best = std::max(best.value_or(0.0), fps);
            }
            row.presented_fps = best; // Never add independent swap-chain frame rates.
        }
    } catch (...) {
        // FPS must never crash resource monitoring if PresentMon becomes unavailable.
        disconnect();
        next_connect_attempt_ = GetTickCount64() + 30000;
    }
}
} // namespace fluxtop
