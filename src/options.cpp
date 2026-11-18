#include "fluxtop/options.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace fluxtop {
Options parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--help" || arg == "-h") options.help = true;
        else if (arg == "--version") options.version = true;
        else if (arg == "--no-gpu") options.gpu = false;
        else if (arg == "--jsonl") options.jsonl = true;
        else if (arg == "--sort" || arg == "--samples" || arg == "--interval-ms" || arg == "--top") {
            if (++i == args.size()) throw std::invalid_argument("Missing value for " + std::string(arg));
            const auto value = args[i];
            if (arg == "--sort") {
                if (value == "cpu") options.sort = Sort::cpu;
                else if (value == "ram") options.sort = Sort::ram;
                else if (value == "gpu") options.sort = Sort::gpu;
                else if (value == "vram") options.sort = Sort::vram;
                else throw std::invalid_argument("Sort must be cpu, ram, gpu or vram");
            } else {
                unsigned number{};
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
                if (error != std::errc{} || end != value.data() + value.size() || value.empty())
                    throw std::invalid_argument("Invalid unsigned integer for " + std::string(arg));
                if (arg == "--interval-ms") {
                    if (number < 250 || number > 60000)
                        throw std::invalid_argument("Interval must be 250..60000 ms");
                    options.interval_ms = number;
                } else if (arg == "--samples") options.samples = number;
                else options.top = number;
            }
        } else throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
    if (!options.gpu && (options.sort == Sort::gpu || options.sort == Sort::vram))
        throw std::invalid_argument("GPU sorting requires the GPU collector");
    return options;
}

std::string_view usage() {
    return "FluxTop - Windows process resource collector\n"
           "Usage: fluxtop [options]\n"
           "  --interval-ms N  Sampling interval, 250..60000 (default: 1000)\n"
           "  --samples N      Stop after N snapshots; 0 runs until Ctrl+C\n"
           "  --top N          Rows per snapshot; 0 shows all (default: 10)\n"
           "  --sort KEY       cpu | ram | gpu | vram (default: cpu)\n"
           "  --no-gpu         Do not initialize or collect PDH counters\n"
           "  --jsonl          UTF-8 JSON Lines, diagnostics on stderr\n"
           "  --help, -h       Show help\n"
           "  --version        Show version\n"
           "Unknown metrics: '-' in text, null in JSON. CPU: whole-machine percent.\n";
}
} // namespace fluxtop
