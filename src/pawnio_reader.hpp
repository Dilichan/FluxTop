#pragma once
#include "fluxtop/windows.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fluxtop {
class PawnIoReader {
public:
    bool ready() const noexcept {return device_.valid();}
    bool open() noexcept {
        device_=Handle(CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        return device_.valid();
    }
    bool load(const wchar_t* module) const {
        std::wstring path(32768,L'\0');
        const DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if(!length || length>=path.size())return false;
        path.resize(length);
        std::ifstream file(std::filesystem::path(path).parent_path()/module,std::ios::binary|std::ios::ate);
        if(!file)return false;
        const auto size=file.tellg();
        if(size<=0 || size>65536)return false;
        auto bytes=std::make_unique<char[]>(static_cast<std::size_t>(size));
        file.seekg(0);
        if(!file.read(bytes.get(),size))return false;
        DWORD returned{};
        return DeviceIoControl(device_.get(),(41394UL<<16)|(0x821UL<<2),bytes.get(),
            static_cast<DWORD>(size),nullptr,0,&returned,nullptr)!=FALSE;
    }
    template<std::size_t N>
    bool read(const char (&function)[N],std::uint64_t address,std::uint64_t& value) const noexcept {
        static_assert(N<=32);
        std::array<std::uint8_t,40> input{};
        std::memcpy(input.data(),function,N);
        std::memcpy(input.data()+32,&address,sizeof(address));
        DWORD returned{};
        value=0;
        return DeviceIoControl(device_.get(),(41394UL<<16)|(0x841UL<<2),input.data(),
            static_cast<DWORD>(input.size()),&value,sizeof(value),&returned,nullptr)!=FALSE &&
            returned==sizeof(value);
    }
    void close() noexcept {device_=Handle{};}
private:
    Handle device_;
};
}
