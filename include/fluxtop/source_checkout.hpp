#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fluxtop::source_checkout {
namespace fs=std::filesystem;
struct Result { int exit_code{-1}; bool timed_out{}; std::string output; };
// Runs a process without a command shell; returns its exit status and captured output.
Result process(const fs::path& exe,const std::vector<std::wstring>& args,
               const fs::path& directory,unsigned timeout_seconds);
// Updates only a clean fast-forward checkout; returns a stable reason string.
std::string update(const fs::path& repository);
// Returns a commit hash only for a clean checkout; otherwise an empty string.
std::string clean_commit(const fs::path& repository);
// Validates and resolves a prior verified build inside the launcher state directory.
fs::path last_good(const fs::path& state);
// Builds and tests in an isolated directory, then atomically publishes last-good.json.
fs::path build_and_publish(const fs::path& repository,const fs::path& state,
                           const std::string& version);
// Returns the desired tested monitor path, keeping a prior working build on failure.
fs::path prepare(const fs::path& repository,const fs::path& state,
                 bool no_update,bool strict_prepare);
// Returns a source checkout by walking upwards from an executable location.
fs::path locate_repository(const fs::path& executable);
// Returns the exact Windows argument quoting for a single argument.
std::wstring quote(std::wstring_view value);
}
