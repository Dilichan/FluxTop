#include "fluxtop/temperatures.hpp"
#include "fluxtop/intel_dts.hpp"
#include "fluxtop/diagnostics.hpp"
#include <wbemidl.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <thread>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <string_view>

namespace fluxtop {
namespace {
#pragma pack(push,1)
struct Header {
    char signature[4];
    std::uint32_t version, revision;
    std::int64_t poll_time;
    std::uint32_t sensor_offset, sensor_stride, sensor_count;
    std::uint32_t reading_offset, reading_stride, reading_count, poll_period;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 48);
std::string ascii_lower(const char* ptr, std::size_t limit) {
    std::size_t len=0;
    while (len<limit && ptr[len]) ++len;
    std::string result(ptr,ptr+len);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
bool within(std::size_t offset, std::size_t stride, std::size_t count, std::size_t end) {
    return offset <= end && stride && count <= (end-offset)/stride;
}
} // namespace

bool cpu_temperature_valid(double celsius) noexcept {
    return std::isfinite(celsius) && celsius>=-10.0 && celsius<=125.0;
}
bool cpu_temperature_fresh(std::int64_t polled_at,std::int64_t now,std::int64_t max_age) noexcept {
    // Reject clock rollbacks/future timestamps and expired readings.
    return polled_at>0 && max_age>=0 && now>=polled_at && now-polled_at<=max_age;
}
namespace {
bool has(std::string_view text,std::string_view token) noexcept {return text.find(token)!=std::string_view::npos;}
bool board_group(std::string_view group) noexcept {
    return has(group,"mainboard") || has(group,"motherboard") || has(group,"super i/o") ||
        has(group,"superio") || has(group,"nuvoton") || has(group,"ite ") ||
        has(group,"embedded controller") || has(group,"lpc") || has(group,"ec sensor");
}
}
int hwinfo_cpu_sensor_priority(std::string_view group,std::string_view label) noexcept {
    if(board_group(group) || has(group,"gpu") || has(group,"graphics") ||
        (!has(group,"cpu") && !has(group,"processor")))return 0;
    if(has(label,"tjmax") || has(label,"distance") || has(label,"limit"))return 0;
    if(has(label,"cpu package") || label=="package")return 5;
    if(has(label,"tctl/tdie"))return 4;
    if(has(label,"cpu die") || label=="tdie")return 3;
    if(has(label,"core average"))return 2;
    if(has(label,"core #") || has(label,"core "))return 1;
    return 0;
}
int hwinfo_board_sensor_priority(std::string_view group,std::string_view label) noexcept {
    if(has(group,"gpu") || has(group,"graphics") || !board_group(group))return 0;
    if(has(label,"vrm") || has(label,"pch") || has(label,"ambient"))return 0;
    if(has(label,"cpu socket"))return 4;
    if(has(label,"cpu (peci)"))return 3;
    return label=="cpu" || label=="cpu temperature" ? 2 : 0;
}
bool acpi_cpu_zone_candidate(std::wstring_view identifier) noexcept {
    std::wstring value(identifier);
    for(auto& c:value)c=static_cast<wchar_t>(std::towlower(c));
    if(value.find(L"vrm")!=std::wstring::npos || value.find(L"pch")!=std::wstring::npos ||
        value.find(L"gpu")!=std::wstring::npos)return false;
    return value.find(L"cpu")!=std::wstring::npos ||
        value.find(L"processor")!=std::wstring::npos;
}
bool cpu_temperature_should_replace(bool current_known,bool current_board,bool incoming_board) noexcept {
    return !current_known || (current_board && !incoming_board);
}

HardwareTemperatures read_hwinfo_temperatures() {
    HardwareTemperatures result;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!mapping) { result.hwinfo_status="mapping-unavailable"; return result; }
    const auto* data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    if (!data) { CloseHandle(mapping); result.hwinfo_status="mapping-inaccessible"; return result; }
    result.hwinfo_status="invalid-header-or-layout";
    do {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(data,&info,sizeof(info)) || info.RegionSize < sizeof(Header)) break;
        const std::size_t total = info.RegionSize;
        Header header{};
        std::memcpy(&header,data,sizeof(header));
        if (std::memcmp(header.signature,"HWiS",4) || header.version == 0 || header.version > 2) break;
        const std::time_t now = std::time(nullptr);
        if (!cpu_temperature_fresh(header.poll_time,static_cast<std::int64_t>(now),30)) {
            result.hwinfo_status="stale-or-future-timestamp"; break;
        }
        if (header.sensor_stride < 264 || header.reading_stride < 292
            || !within(header.sensor_offset,header.sensor_stride,header.sensor_count,total)
            || !within(header.reading_offset,header.reading_stride,header.reading_count,total)) break;
        result.hwinfo_status="no-supported-cpu-sensor";
        int cpu_priority=0, gpu_priority=0, board_priority=0;
        std::optional<double> board_temp;
        for (std::size_t index=0;index<header.reading_count;++index) {
            const auto* read = data + header.reading_offset + index*header.reading_stride;
            std::uint32_t type=0, sensor=0;
            std::memcpy(&type,read,4); std::memcpy(&sensor,read+4,4);
            if (type!=1 || sensor>=header.sensor_count) continue;
            const auto* source = data + header.sensor_offset + static_cast<std::size_t>(sensor)*header.sensor_stride;
            const auto group = ascii_lower(reinterpret_cast<const char*>(source+8),128);
            const auto label = ascii_lower(reinterpret_cast<const char*>(read+12),128);
            double temperature=0;
            std::memcpy(&temperature,read+284,sizeof(temperature));
            if (!cpu_temperature_valid(temperature)) continue;
            const bool gpu = group.find("gpu")!=std::string::npos;
            const int cp = hwinfo_cpu_sensor_priority(group,label);
            const int gp = label.find("gpu temperature")!=std::string::npos ? 3
                : label.find("gpu core")!=std::string::npos && label.find("temperature")!=std::string::npos ? 2 : 0;
            if (cp>cpu_priority) { result.cpu_c=temperature; cpu_priority=cp; }
            const int bp = hwinfo_board_sensor_priority(group,label);
            if (bp > board_priority) { board_priority=bp; board_temp=temperature; }
            if (gpu && gp>gpu_priority) { result.gpu_c=temperature; gpu_priority=gp; }
        }
        if (!result.cpu_c && board_temp) { result.cpu_c=board_temp; result.cpu_from_board=true; }
        if (result.cpu_c) {
            result.cpu_source=result.cpu_from_board?"hwinfo-board":"hwinfo-die";
            result.hwinfo_status="ready";
        }
    } while(false);
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return result;
}

// Select only an actual CPU die/package sensor exposed by a hardware monitor.
// Board, socket and ACPI thermal-zone sensors must not be mislabeled as CPU temperature.
int cpu_temperature_sensor_priority(std::wstring_view identifier, std::wstring_view name) noexcept {
    const auto lower = [](std::wstring_view src) {
        std::wstring s(src);
        for (auto& c : s) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        return s;
    };
    const auto id = lower(identifier), label = lower(name);
    if (id.find(L"/intelcpu/") == std::wstring::npos
        && id.find(L"/amdcpu/") == std::wstring::npos
        && id.find(L"/cpu/") == std::wstring::npos)
        return 0;
    if (label.find(L"cpu package") != std::wstring::npos || label == L"package") return 5;
    if (label.find(L"tctl/tdie") != std::wstring::npos) return 4;
    if (label.find(L"cpu die") != std::wstring::npos || label.find(L"tdie") != std::wstring::npos) return 3;
    if (label.find(L"core average") != std::wstring::npos) return 2;
    if (label.find(L"core #") != std::wstring::npos || label.find(L"core ") != std::wstring::npos) return 1;
    return 0;
}

int motherboard_cpu_sensor_priority(std::wstring_view identifier, std::wstring_view name) noexcept {
    const auto lower=[](std::wstring_view src) {
        std::wstring result(src);
        for(auto& c:result) c=static_cast<wchar_t>(std::towlower(c));
        return result;
    };
    const auto id=lower(identifier), label=lower(name);
    if ((id.find(L"/mainboard/")==std::wstring::npos
        && id.find(L"/motherboard/")==std::wstring::npos
        && id.find(L"/lpc/")==std::wstring::npos
        && id.find(L"/superio/")==std::wstring::npos
        && id.find(L"/ec/")==std::wstring::npos)
        || id.find(L"/temperature/")==std::wstring::npos) return 0;
    if(label.find(L"cpu package")!=std::wstring::npos) return 5; // Board-reported, not die sensor.
    if(label.find(L"cpu socket")!=std::wstring::npos) return 4;
    if(label.find(L"cpu (peci)")!=std::wstring::npos) return 3;
    if(label==L"cpu" || label==L"cpu temperature" || label==L"processor") return 2;
    return 0;
}
namespace {
using Microsoft::WRL::ComPtr;

// Native WMI interfaces: no .NET runtime, PowerShell or vendor-specific DLL.
struct ScopedBstr {
    BSTR value{};
    explicit ScopedBstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ~ScopedBstr() { SysFreeString(value); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
};
std::optional<std::wstring> wmi_text(IWbemClassObject* obj, const wchar_t* property) {
    VARIANT value{}; VariantInit(&value);
    const HRESULT hr = obj->Get(property,0,&value,nullptr,nullptr);
    std::optional<std::wstring> result;
    if (SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal)
        result = std::wstring(value.bstrVal,SysStringLen(value.bstrVal));
    VariantClear(&value);
    return result;
}
std::optional<double> wmi_number(IWbemClassObject* obj, const wchar_t* property) {
    VARIANT value{}; VariantInit(&value);
    const HRESULT hr = obj->Get(property,0,&value,nullptr,nullptr);
    std::optional<double> result;
    if (SUCCEEDED(hr)) {
        if (value.vt == VT_R4) result = static_cast<double>(value.fltVal);
        else if (value.vt == VT_R8) result = value.dblVal;
        else if (value.vt == VT_I4) result = static_cast<double>(value.lVal);
        else if (value.vt == VT_UI4) result = static_cast<double>(value.ulVal);
        else if (value.vt == VT_I8) result = static_cast<double>(value.llVal);
        else if (value.vt == VT_UI8) result = static_cast<double>(value.ullVal);
    }
    VariantClear(&value);
    return result;
}
struct CpuReading { double celsius{}; bool board{}; unsigned source{}; };
std::optional<CpuReading> read_wmi_cpu_namespace(IWbemLocator* locator, const wchar_t* name, unsigned source) {
    ScopedBstr scope(name), language(L"WQL");
    ScopedBstr sql(L"SELECT Name, Identifier, Value FROM Sensor WHERE SensorType='Temperature'");
    if (!scope.value || !language.value || !sql.value) return std::nullopt;
    ComPtr<IWbemServices> services;
    if (FAILED(locator->ConnectServer(scope.value,nullptr,nullptr,nullptr,0,nullptr,nullptr,&services)))
        return std::nullopt; // Provider not installed or inaccessible.
    if (FAILED(CoSetProxyBlanket(services.Get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,
        RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE))) return std::nullopt;
    ComPtr<IEnumWbemClassObject> enumerator;
    if (FAILED(services->ExecQuery(language.value,sql.value,WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,&enumerator))) return std::nullopt;
    int best_priority=0, best_board_priority=0;
    std::optional<double> best, best_board;
    // Bound the total enumeration time as well as the sample count.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for (unsigned i=0;i<512 && std::chrono::steady_clock::now()<deadline;++i) {
        ComPtr<IWbemClassObject> sensor;
        ULONG count=0;
        IWbemClassObject* raw{};
        const HRESULT hr=enumerator->Next(100,1,&raw,&count);
        sensor.Attach(raw);
        if (hr != WBEM_S_NO_ERROR || count != 1 || !sensor) break;
        const auto identifier=wmi_text(sensor.Get(),L"Identifier");
        const auto label=wmi_text(sensor.Get(),L"Name");
        const auto temp=wmi_number(sensor.Get(),L"Value");
        if (!identifier || !label || !temp || !cpu_temperature_valid(*temp)) continue;
        const int priority=cpu_temperature_sensor_priority(*identifier,*label);
        if (priority>best_priority) { best_priority=priority; best=*temp; }
        const int board_priority=motherboard_cpu_sensor_priority(*identifier,*label);
        if (board_priority>best_board_priority) { best_board_priority=board_priority; best_board=*temp; }
    }
    if(best) return CpuReading{*best,false,source};
    if(best_board) return CpuReading{*best_board,true,source};
    return std::nullopt;
}

// ACPI zones are firmware-defined; accept only zones whose identifiers explicitly
// reference CPU/processor. Unknown TZ00/TZ01 and platform zones are not CPU sensors.
std::optional<CpuReading> read_acpi_cpu_zone(IWbemLocator* locator) {
    ScopedBstr scope(L"ROOT\\WMI"), language(L"WQL");
    ScopedBstr sql(L"SELECT InstanceName, CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    if(!scope.value || !language.value || !sql.value) return std::nullopt;
    ComPtr<IWbemServices> services;
    if(FAILED(locator->ConnectServer(scope.value,nullptr,nullptr,nullptr,0,nullptr,nullptr,&services)))
        return std::nullopt;
    if(FAILED(CoSetProxyBlanket(services.Get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,
        RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE))) return std::nullopt;
    ComPtr<IEnumWbemClassObject> enumerator;
    if(FAILED(services->ExecQuery(language.value,sql.value,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&enumerator))) return std::nullopt;
    for(unsigned i=0;i<64;++i){
        IWbemClassObject* raw{}; ULONG count{};
        const HRESULT hr=enumerator->Next(350,1,&raw,&count);
        ComPtr<IWbemClassObject> sensor; sensor.Attach(raw);
        if(hr!=WBEM_S_NO_ERROR || count!=1 || !sensor) break;
        const auto id=wmi_text(sensor.Get(),L"InstanceName");
        const auto raw_temp=wmi_number(sensor.Get(),L"CurrentTemperature");
        if(!id || !raw_temp) continue;
        auto label=*id;
        for(auto& c:label) c=static_cast<wchar_t>(std::towlower(c));
        if(!acpi_cpu_zone_candidate(label))continue;
        const double celsius=*raw_temp/10.0-273.15; // ACPI stores 0.1 kelvin.
        if(cpu_temperature_valid(celsius))
            return CpuReading{celsius,true,3};
    }
    return std::nullopt;
}
std::optional<CpuReading> read_wmi_cpu() {
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(init)) return std::nullopt;
    struct Uninit { ~Uninit() { CoUninitialize(); } } uninit;
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&locator))))
        return std::nullopt;
    // Prefer CPU die/package regardless of provider. Board CPU zone is fallback.
    const auto libre=read_wmi_cpu_namespace(locator.Get(),L"ROOT\\LibreHardwareMonitor",1);
    if(libre && !libre->board) return libre;
    const auto open=read_wmi_cpu_namespace(locator.Get(),L"ROOT\\OpenHardwareMonitor",2);
    if(open && !open->board) return open;
    if(libre) return libre;
    if(open) return open;
    return read_acpi_cpu_zone(locator.Get());
}

// WMI is potentially slow: never perform COM queries on the sampler/UI thread.
// A cached value expires after 20 seconds, so stopped providers cannot leave a frozen temperature.
class WmiCpuCache {
public:
    WmiCpuCache() : worker_([this](std::stop_token stop){ loop(stop); }) {}
    std::optional<CpuReading> latest() const {
        const auto sample=last_ms_.load(std::memory_order_acquire);
        if (!sample || GetTickCount64()-sample>20000) return std::nullopt;
        const auto value=celsius_.load(std::memory_order_relaxed);
        if (!std::isfinite(value)) return std::nullopt;
        return CpuReading{value,board_.load(std::memory_order_relaxed),source_.load(std::memory_order_relaxed)};
    }
private:
    void loop(std::stop_token stop) {
        using namespace std::chrono_literals;
        while (!stop.stop_requested()) {
            const auto value=read_wmi_cpu();
            if (value) {
                celsius_.store(value->celsius,std::memory_order_relaxed);
                board_.store(value->board,std::memory_order_relaxed);
                source_.store(value->source,std::memory_order_relaxed);
                last_ms_.store(GetTickCount64(),std::memory_order_release);
            } else last_ms_.store(0,std::memory_order_release);
            const unsigned ticks=value ? 10U : 40U; // 5s when active, 20s probing fallback.
            for (unsigned i=0; i<ticks && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(500ms);
        }
    }
    std::atomic<double> celsius_{0.0};
    std::atomic<bool> board_{false};
    std::atomic<unsigned> source_{0};
    std::atomic<ULONGLONG> last_ms_{0};
    std::jthread worker_;
};
} // namespace

HardwareTemperatures read_hardware_temperatures() {
    auto result=read_hwinfo_temperatures();
    const auto dts=read_intel_dts();
    if(dts.package_c){
        result.cpu_c=dts.package_c;
        result.cpu_from_board=false;
        result.cpu_source="intel-dts-pawnio";
    }
    if (!result.cpu_c || result.cpu_from_board) {
        static WmiCpuCache cpu_wmi;
        if(const auto value=cpu_wmi.latest()) {
            if(cpu_temperature_should_replace(result.cpu_c.has_value(),result.cpu_from_board,value->board)) {
                result.cpu_c=value->celsius;
                result.cpu_from_board=value->board;
                const char* provider=value->source==1?"libre-hardware-monitor":
                    value->source==2?"open-hardware-monitor":
                    value->source==3?"acpi":"unknown-wmi";
                result.cpu_source=std::string(provider)+(value->board?"-board":"-die");
            }
        }
    }
    // Emit a transition only, not a log line per sampling interval.
    static std::mutex status_mutex;
    static std::string last_source, last_reason;
    const std::string reason=result.cpu_c?"ready":
        std::string("no-valid-cpu-temperature; hwinfo=")+result.hwinfo_status+
        "; wmi-provider-or-sensor-unavailable";
    {
        std::lock_guard guard(status_mutex);
        if(last_source!=result.cpu_source || last_reason!=reason) {
            last_source=result.cpu_source;
            last_reason=reason;
            diagnostics::write(result.cpu_c?diagnostics::Level::info:diagnostics::Level::warning,
                "temperature.cpu", "source="+result.cpu_source+" status="+reason);
        }
    }
    return result;
}
} // namespace fluxtop