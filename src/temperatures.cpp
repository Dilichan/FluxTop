#include "fluxtop/temperatures.hpp"
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
HardwareTemperatures read_hwinfo_temperatures() {
    HardwareTemperatures result;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    if (!mapping) return result;
    const auto* data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    if (!data) { CloseHandle(mapping); return result; }
    do {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(data,&info,sizeof(info)) || info.RegionSize < sizeof(Header)) break;
        const std::size_t total = info.RegionSize;
        Header header{};
        std::memcpy(&header,data,sizeof(header));
        if (std::memcmp(header.signature,"HWiS",4) || header.version == 0 || header.version > 2) break;
        const std::time_t now = std::time(nullptr);
        if (header.poll_time <= 0 || std::abs(static_cast<double>(now) - static_cast<double>(header.poll_time)) > 30) break;
        if (header.sensor_stride < 264 || header.reading_stride < 292
            || !within(header.sensor_offset,header.sensor_stride,header.sensor_count,total)
            || !within(header.reading_offset,header.reading_stride,header.reading_count,total)) break;
        int cpu_priority=0, gpu_priority=0;
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
            if (!std::isfinite(temperature) || temperature < 10 || temperature > 115) continue;
            const bool cpu = group.find("cpu")!=std::string::npos || group.find("processor")!=std::string::npos;
            const bool gpu = group.find("gpu")!=std::string::npos;
            const int cp = label.find("cpu package")!=std::string::npos ? 3
                : label.find("tctl/tdie")!=std::string::npos ? 3
                : label.find("cpu die")!=std::string::npos ? 2 : 0;
            const int gp = label.find("gpu temperature")!=std::string::npos ? 3
                : label.find("gpu core")!=std::string::npos && label.find("temperature")!=std::string::npos ? 2 : 0;
            if (cpu && cp>cpu_priority) { result.cpu_c=temperature; cpu_priority=cp; }
            if (gpu && gp>gpu_priority) { result.gpu_c=temperature; gpu_priority=gp; }
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
    }
    VariantClear(&value);
    return result;
}
std::optional<double> read_wmi_cpu_namespace(IWbemLocator* locator, const wchar_t* name) {
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
    int best_priority=0;
    std::optional<double> best;
    // Hard bounds prevent a broken provider from monopolizing the sampler thread.
    for (unsigned i=0;i<512;++i) {
        ComPtr<IWbemClassObject> sensor;
        ULONG count=0;
        IWbemClassObject* raw{};
        const HRESULT hr=enumerator->Next(350,1,&raw,&count);
        sensor.Attach(raw);
        if (hr != WBEM_S_NO_ERROR || count != 1 || !sensor) break;
        const auto identifier=wmi_text(sensor.Get(),L"Identifier");
        const auto label=wmi_text(sensor.Get(),L"Name");
        const auto temp=wmi_number(sensor.Get(),L"Value");
        if (!identifier || !label || !temp || !std::isfinite(*temp) || *temp < -10 || *temp > 125) continue;
        const int priority=cpu_temperature_sensor_priority(*identifier,*label);
        if (priority>best_priority) { best_priority=priority; best=*temp; }
    }
    return best;
}
std::optional<double> read_wmi_cpu() {
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(init)) return std::nullopt;
    struct Uninit { ~Uninit() { CoUninitialize(); } } uninit;
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&locator))))
        return std::nullopt;
    // Match CPU sensors on Intel, AMD and future CPUs via provider sensor IDs.
    if (const auto value=read_wmi_cpu_namespace(locator.Get(),L"ROOT\\LibreHardwareMonitor")) return value;
    return read_wmi_cpu_namespace(locator.Get(),L"ROOT\\OpenHardwareMonitor");
}

// WMI is potentially slow: never perform COM queries on the sampler/UI thread.
// A cached value expires after 20 seconds, so stopped providers cannot leave a frozen temperature.
class WmiCpuCache {
public:
    WmiCpuCache() : worker_([this](std::stop_token stop){ loop(stop); }) {}
    std::optional<double> latest() const {
        const auto sample=last_ms_.load(std::memory_order_acquire);
        if (!sample || GetTickCount64()-sample>20000) return std::nullopt;
        const auto value=celsius_.load(std::memory_order_relaxed);
        if (!std::isfinite(value)) return std::nullopt;
        return value;
    }
private:
    void loop(std::stop_token stop) {
        using namespace std::chrono_literals;
        while (!stop.stop_requested()) {
            const auto value=read_wmi_cpu();
            if (value) {
                celsius_.store(*value,std::memory_order_relaxed);
                last_ms_.store(GetTickCount64(),std::memory_order_release);
            } else last_ms_.store(0,std::memory_order_release);
            const unsigned ticks=value ? 10U : 40U; // 5s when active, 20s probing fallback.
            for (unsigned i=0; i<ticks && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(500ms);
        }
    }
    std::atomic<double> celsius_{0.0};
    std::atomic<ULONGLONG> last_ms_{0};
    std::jthread worker_;
};
} // namespace

HardwareTemperatures read_hardware_temperatures() {
    auto result=read_hwinfo_temperatures();
    if (!result.cpu_c) {
        static WmiCpuCache cpu_wmi;
        result.cpu_c=cpu_wmi.latest();
    }
    return result;
}
} // namespace fluxtop