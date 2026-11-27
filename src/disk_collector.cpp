#include "fluxtop/disk_collector.hpp"
#include <windows.h>
#include "fluxtop/disk_temperature.hpp"
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace fluxtop {
DiskCollector::~DiskCollector() {
    if (query_) PdhCloseQuery(query_);
}
void DiskCollector::refresh(DWORD drive_mask) {
    if (query_) { PdhCloseQuery(query_); query_=nullptr; }
    counters_.clear();
    initialized_=true;
    last_mask_=drive_mask;
    // Only real mounted file-system drives; empty optical slots are excluded.
    for (unsigned i=0;i<26;++i) {
        if (!(drive_mask & (1UL<<i))) continue;
        const wchar_t letter=static_cast<wchar_t>(L'A'+i);
        const wchar_t root[4]{letter,L':',L'\\',L'\0'};
        const UINT type=GetDriveTypeW(root);
        if (type!=DRIVE_FIXED && type!=DRIVE_REMOVABLE) continue;
        counters_.push_back({letter,{}});
    }
    if (counters_.empty() || PdhOpenQueryW(nullptr,0,&query_)!=ERROR_SUCCESS) {
        query_=nullptr;return;
    }
    for (auto& counter:counters_) {
        std::wstring path=L"\\LogicalDisk(";
        path+=counter.letter;
        path+=L":)\\% Disk Time";
        if(PdhAddEnglishCounterW(query_,path.c_str(),0,&counter.handle)!=ERROR_SUCCESS)
            counter.handle=nullptr; // Detected drive remains visible as unavailable.
    }
    // PDH rate counters require two readings. Never report the first as zero.
    PdhCollectQueryData(query_);
}
void DiskCollector::collect(Snapshot& snapshot,DWORD mask) {
    if(!initialized_ || mask!=last_mask_) refresh(mask);
    snapshot.disks.clear();
    snapshot.disks.reserve(counters_.size());
    const auto temperatures=latest_disk_temperatures();
    const bool ready=query_ && PdhCollectQueryData(query_)==ERROR_SUCCESS;
    for(const auto& counter:counters_) {
        DiskActivity sample{};
        sample.letter=counter.letter;
        sample.temperature_c=temperatures[static_cast<unsigned>(counter.letter-L'A')];
        if(ready && counter.handle) {
            PDH_FMT_COUNTERVALUE value{};
            DWORD type{};
            if(PdhGetFormattedCounterValue(counter.handle,PDH_FMT_DOUBLE,&type,&value)==ERROR_SUCCESS &&
                (value.CStatus==PDH_CSTATUS_VALID_DATA || value.CStatus==PDH_CSTATUS_NEW_DATA) &&
                std::isfinite(value.doubleValue))
                sample.active_percent=std::clamp(value.doubleValue,0.0,100.0);
        }
        snapshot.disks.push_back(sample);
    }
}
} // namespace fluxtop
