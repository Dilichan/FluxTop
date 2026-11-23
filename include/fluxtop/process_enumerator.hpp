#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fluxtop {
struct ProcessEntry { std::uint32_t pid{},parent_pid{};std::string name; };
// The documented x64 SystemBasicProcessInformation ABI. Keep a local spelling
// so older Windows SDKs can build the runtime-probed Windows 11 optimization.
// https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
struct BasicProcessRecord {
    ULONG next{};HANDLE pid{},parent{};ULONGLONG sequence{};
    USHORT name_bytes{},maximum_name_bytes{};PWSTR name{};
};
static_assert(sizeof(BasicProcessRecord)==48 && offsetof(BasicProcessRecord,name)==40);
bool parse_basic_processes(std::span<const std::byte> bytes,std::vector<ProcessEntry>& entries);
class ProcessEnumerator {
public:
    using Query=LONG(NTAPI*)(ULONG,void*,ULONG,ULONG*);
    using Fallback=std::vector<ProcessEntry>(*)();
    ProcessEnumerator();
    ProcessEnumerator(Query query,Fallback fallback):query_(query),fallback_(fallback){}
    std::vector<ProcessEntry> collect(std::uint64_t now_ms);
private:
    Query query_{};
    Fallback fallback_{};
    std::vector<std::byte> buffer_;
    bool unsupported_{};
    std::uint64_t retry_at_{};
};
}
