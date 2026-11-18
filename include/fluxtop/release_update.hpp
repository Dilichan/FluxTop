#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <functional>
namespace fluxtop {
struct Release { std::wstring tag; std::string digest; std::size_t size{}; };
bool newer_release(std::wstring_view candidate,std::wstring_view current);
std::optional<Release> release_from_json(std::wstring_view json,std::wstring_view current);
std::string release_sha256(const std::vector<unsigned char>& bytes);
// Check published metadata only. No download or installation.
std::optional<Release> find_release_update(std::wstring_view current);
// Download an explicitly selected release. Activation is a separate operation.
std::vector<unsigned char> download_release_update(const Release& release);
bool activate_release(const std::filesystem::path& directory,const std::vector<unsigned char>& bytes,
    const Release& release,const std::function<bool(const std::filesystem::path&)>& start);
bool try_release_update(const std::filesystem::path& directory,const std::filesystem::path& self,
    std::wstring_view current,bool tray) noexcept;
}
