#include "fluxtop/gpu_collector.hpp"
#include "fluxtop/options.hpp"
#include "fluxtop/overlay.hpp"
#include "fluxtop/output.hpp"
#include "fluxtop/process_collector.hpp"
#include "fluxtop/privileges.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/sampler.hpp"

#include <psapi.h>
#include <chrono>
#include <iostream>
#include <locale>

namespace {
HANDLE stop_event = nullptr;
BOOL WINAPI stop_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}
class StopHandler {
public:
    explicit StopHandler(HANDLE event) {
        stop_event = event;
        if (!SetConsoleCtrlHandler(stop_handler, TRUE)) throw fluxtop::windows_error("SetConsoleCtrlHandler");
    }
    ~StopHandler() { SetConsoleCtrlHandler(stop_handler, FALSE); }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> storage;
        storage.reserve(static_cast<std::size_t>(argc));
        for (int i = 1; i < argc; ++i) storage.push_back(fluxtop::utf8(argv[i]));
        std::vector<std::string_view> args(storage.begin(), storage.end());
        const auto options = fluxtop::parse_options(args);
        if (options.help) { std::cout << fluxtop::usage(); return 0; }
        if (options.version) { std::cout << "FluxTop " << FLUXTOP_VERSION << '\n'; return 0; }
        if (options.overlay) return fluxtop::run_overlay(options);
        std::cout.imbue(std::locale::classic());
        // Windows console uses UTF-8 during this process; restore its previous code page on exit.
        struct ConsoleEncoding {
            UINT previous = GetConsoleOutputCP();
            ConsoleEncoding() { if (previous) SetConsoleOutputCP(CP_UTF8); }
            ~ConsoleEncoding() { if (previous) SetConsoleOutputCP(previous); }
        } encoding;
        fluxtop::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) throw fluxtop::windows_error("CreateEventW");
        StopHandler handler(event.get());
        fluxtop::Sampler sampler(options.gpu, options.network);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.interval_ms);
        unsigned emitted = 0;
        while (!options.samples || emitted < options.samples) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            const auto wait = WaitForSingleObject(event.get(), remaining > 0 ? static_cast<DWORD>(remaining) : 0);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_TIMEOUT) throw fluxtop::windows_error("WaitForSingleObject");
            const auto start = std::chrono::steady_clock::now();
            auto snapshot = sampler.collect();
            fluxtop::sort_processes(snapshot.processes, options.sort);
            if (options.jsonl) fluxtop::write_jsonl(std::cout, snapshot, options.top);
            else fluxtop::write_table(std::cout, snapshot, options.top);
            std::cout.flush();
            if (!std::cout) throw std::runtime_error("Cannot write output");
            ++emitted;
            deadline = start + std::chrono::milliseconds(options.interval_ms);
            // After an overrun, wait a full interval rather than spinning to catch up.
            if (deadline <= std::chrono::steady_clock::now())
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.interval_ms);
        }
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "FluxTop: " << error.what() << "\nUse --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "FluxTop: " << error.what() << '\n';
        return 1;
    }
}
