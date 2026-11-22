#include "fluxtop/disk_temperature.hpp"
#include <windows.h>
#include <winioctl.h>
#include <array>
#include <string>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstddef>
#include <cstdint>
namespace fluxtop {
namespace {
using Temperatures=std::array<std::optional<double>,26>;
struct Handle {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit Handle(const wchar_t* path):handle(CreateFileW(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,0,nullptr)){}
    ~Handle(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    bool valid()const{return handle!=INVALID_HANDLE_VALUE;}
};
std::optional<double> physical_temperature(DWORD disk_number) {
    const auto path=std::wstring(L"\\\\.\\PhysicalDrive")+std::to_wstring(disk_number);
    Handle disk(path.c_str());
    if(!disk.valid())return std::nullopt;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId=StorageDeviceTemperatureProperty;
    query.QueryType=PropertyStandardQuery;
    alignas(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) std::array<std::byte,512> raw{};
    DWORD bytes{};
    if(!DeviceIoControl(disk.handle,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),
        raw.data(),static_cast<DWORD>(raw.size()),&bytes,nullptr) ||
        bytes<offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR,TemperatureInfo))return std::nullopt;
    const auto* data=reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(raw.data());
    const auto offset=offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR,TemperatureInfo);
    if(data->Size<offset || data->Size>bytes || data->InfoCount==0 ||
       data->InfoCount>(bytes-offset)/sizeof(STORAGE_TEMPERATURE_INFO))return std::nullopt;
    // Sensor 0 is composite when exposed. Otherwise first valid temperature.
    std::optional<double> fallback;
    for(WORD i=0;i<data->InfoCount;++i){
        const auto& sensor=data->TemperatureInfo[i];
        const double value=sensor.Temperature;
        if(value< -10 || value>125)continue;
        if(sensor.Index==0)return value;
        if(!fallback)fallback=value;
    }
    return fallback;
}
std::optional<DWORD> volume_disk(wchar_t letter) {
    const wchar_t path[]{L'\\',L'\\',L'.',L'\\',letter,L':',0};
    Handle volume(path);
    if(!volume.valid())return std::nullopt;
    std::array<std::byte,2048> raw{};
    DWORD bytes{};
    if(!DeviceIoControl(volume.handle,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,
        raw.data(),static_cast<DWORD>(raw.size()),&bytes,nullptr) ||
        bytes<offsetof(VOLUME_DISK_EXTENTS,Extents)+sizeof(DISK_EXTENT))return std::nullopt;
    const auto* extents=reinterpret_cast<const VOLUME_DISK_EXTENTS*>(raw.data());
    if(extents->NumberOfDiskExtents!=1)return std::nullopt;
    return extents->Extents[0].DiskNumber;
}
Temperatures scan() {
    Temperatures values{};
    const DWORD mask=GetLogicalDrives();
    std::array<std::optional<double>,128> cache{};
    std::array<bool,128> checked{};
    for(unsigned i=0;i<26;++i) {
        if(!(mask&(1u<<i)))continue;
        const auto letter=static_cast<wchar_t>(L'A'+i);
        const wchar_t root[]{letter,L':',L'\\',0};
        if(GetDriveTypeW(root)!=DRIVE_FIXED && GetDriveTypeW(root)!=DRIVE_REMOVABLE)continue;
        const auto disk=volume_disk(letter);
        if(!disk || *disk>=cache.size())continue;
        if(!checked[*disk]){cache[*disk]=physical_temperature(*disk);checked[*disk]=true;}
        values[i]=cache[*disk];
    }
    return values;
}
class Cache {
public:
    Cache():worker_([this](std::stop_token stop){
        using namespace std::chrono_literals;
        while(!stop.stop_requested()){
            const auto values=scan();
            {std::lock_guard guard(mutex_);values_=values;last_=GetTickCount64();}
            for(int i=0;i<20 && !stop.stop_requested();++i)std::this_thread::sleep_for(250ms);
        }
    }){}
    Temperatures latest(){
        std::lock_guard guard(mutex_);
        return last_ && GetTickCount64()-last_<=15000?values_:Temperatures{};
    }
private:
    std::mutex mutex_;
    Temperatures values_{};
    ULONGLONG last_{};
    std::jthread worker_;
};
}
Temperatures latest_disk_temperatures(){static Cache cache;return cache.latest();}
}