#include "fluxtop/process_enumerator.hpp"
#include "fluxtop/windows.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace fluxtop {
namespace {
constexpr ULONG basic_information=252;
constexpr LONG length_mismatch=static_cast<LONG>(0xc0000004u);
constexpr std::size_t maximum_buffer=64U*1024*1024; // Malformed/growing reply guard, not a RAM budget.
std::vector<ProcessEntry> toolhelp_processes(){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(!snapshot.valid())throw windows_error("CreateToolhelp32Snapshot");
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(!Process32FirstW(snapshot.get(),&entry))throw windows_error("Process32FirstW");
    std::vector<ProcessEntry> rows;rows.reserve(512);
    do {if(entry.th32ProcessID)rows.push_back({entry.th32ProcessID,entry.th32ParentProcessID,utf8(entry.szExeFile)});}
    while(Process32NextW(snapshot.get(),&entry));
    if(GetLastError()!=ERROR_NO_MORE_FILES)throw windows_error("Process32NextW");
    return rows;
}
}
bool parse_basic_processes(std::span<const std::byte> bytes,std::vector<ProcessEntry>& entries){
    std::vector<ProcessEntry> parsed;parsed.reserve(512);
    std::size_t offset=0;
    const auto base=reinterpret_cast<std::uintptr_t>(bytes.data());
    while(offset<bytes.size()){
        if(bytes.size()-offset<sizeof(BasicProcessRecord))return false;
        BasicProcessRecord record;std::memcpy(&record,bytes.data()+offset,sizeof(record));
        const auto size=record.next?static_cast<std::size_t>(record.next):bytes.size()-offset;
        if(size<sizeof(record) || size>bytes.size()-offset || (record.next%alignof(BasicProcessRecord)))return false;
        const auto pid=reinterpret_cast<std::uintptr_t>(record.pid),parent=reinterpret_cast<std::uintptr_t>(record.parent);
        if(pid>UINT32_MAX || parent>UINT32_MAX || record.name_bytes%sizeof(wchar_t) || record.name_bytes>record.maximum_name_bytes)return false;
        if(pid){
            const auto pointer=reinterpret_cast<std::uintptr_t>(record.name);
            if(!record.name_bytes || pointer<base+offset+sizeof(record) || pointer>base+offset+size ||
                record.name_bytes>base+offset+size-pointer || pointer%alignof(wchar_t))return false;
            const std::wstring_view name(record.name,record.name_bytes/sizeof(wchar_t));
            if(name.find(L'\0')!=name.npos)return false;
            const auto length=static_cast<int>(name.size());
            const auto utf8_bytes=WideCharToMultiByte(CP_UTF8,0,name.data(),length,nullptr,0,nullptr,nullptr);
            if(utf8_bytes<=0)return false;
            std::string image(static_cast<std::size_t>(utf8_bytes),'\0');
            if(WideCharToMultiByte(CP_UTF8,0,name.data(),length,image.data(),utf8_bytes,nullptr,nullptr)!=utf8_bytes)return false;
            parsed.push_back({static_cast<std::uint32_t>(pid),static_cast<std::uint32_t>(parent),std::move(image)});
        }
        if(!record.next){
            if(parsed.empty())return false;
            std::vector<std::uint32_t> ids;ids.reserve(parsed.size());
            for(const auto& row:parsed)ids.push_back(row.pid);
            std::sort(ids.begin(),ids.end());
            if(std::adjacent_find(ids.begin(),ids.end())!=ids.end())return false;
            entries=std::move(parsed);return true;
        }
        offset+=record.next;
    }
    return false;
}
ProcessEnumerator::ProcessEnumerator():fallback_(toolhelp_processes){
    if(const auto module=GetModuleHandleW(L"ntdll.dll"))
        query_=std::bit_cast<Query>(GetProcAddress(module,"NtQuerySystemInformation"));
}
std::vector<ProcessEntry> ProcessEnumerator::collect(std::uint64_t now_ms){
    if(query_ && !unsupported_ && now_ms>=retry_at_){
        if(buffer_.empty())buffer_.resize(65536);
        for(unsigned attempt=0;attempt<8;++attempt){
            ULONG returned{};const auto status=query_(basic_information,buffer_.data(),static_cast<ULONG>(buffer_.size()),&returned);
            if(status==length_mismatch){
                const auto wanted=std::max(buffer_.size()*2,static_cast<std::size_t>(returned));
                if(wanted>maximum_buffer)break;
                buffer_.resize(wanted);continue;
            }
            if(status>=0 && returned && returned<=buffer_.size()){
                std::vector<ProcessEntry> rows;
                if(parse_basic_processes({buffer_.data(),returned},rows))return rows;
            }
            if(status==static_cast<LONG>(0xc0000003u) || status==static_cast<LONG>(0xc0000002u) || status==static_cast<LONG>(0xc00000bbu)){
                unsupported_=true;std::vector<std::byte>().swap(buffer_);
            }
            break;
        }
        // Failures never publish a truncated native snapshot. Use the existing
        // complete provider immediately and avoid probing again every sample.
        retry_at_=now_ms>std::numeric_limits<std::uint64_t>::max()-30000?std::numeric_limits<std::uint64_t>::max():now_ms+30000;
    }
    return fallback_();
}
}
