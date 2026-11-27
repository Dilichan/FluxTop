#include "fluxtop/app_icons.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winsqlite/winsqlite3.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace fluxtop {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t default_memory_limit = 64;
struct IconHandle { HICON value{}; ~IconHandle() { if (value) DestroyIcon(value); } };
struct Statement {
    sqlite3_stmt* value{};
    ~Statement() { if (value) sqlite3_finalize(value); }
};

class Codec {
public:
    Codec() { CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_)); }
    std::shared_ptr<AppIcon> extract(const std::wstring& path, std::vector<std::byte>& png) {
        if (!factory_) return {};
        IconHandle icon;
        if (!path.empty()) SHDefExtractIconW(path.c_str(), 0, 0, &icon.value, nullptr, MAKELONG(32, 16));
        const bool extracted = icon.value != nullptr;
        if (!icon.value) icon.value = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
        if (!icon.value) return {};
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory_->CreateBitmapFromHICON(icon.value, &bitmap))) return {};
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory_->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), AppIcon::size, AppIcon::size, WICBitmapInterpolationModeFant))) return {};
        auto result = pixels(scaler.Get());
        if (!result || !extracted) return result;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
            FAILED(factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get())) ||
            FAILED(frame->SetSize(AppIcon::size, AppIcon::size))) return result;
        auto format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(scaler.Get(), nullptr)) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) return result;
        STATSTG stat{};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 16384) return result;
        png.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        LARGE_INTEGER start{};
        ULONG read{};
        if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) ||
            FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size()) png.clear();
        return result;
    }
    std::shared_ptr<AppIcon> decode(const void* bytes, int size) {
        if (!factory_ || size <= 0 || size > 16384) return {};
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(factory_->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), static_cast<DWORD>(size))) ||
            FAILED(factory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame))) return {};
        UINT width{}, height{};
        if (FAILED(frame->GetSize(&width, &height)) || width != AppIcon::size || height != AppIcon::size) return {};
        return pixels(frame.Get());
    }
private:
    std::shared_ptr<AppIcon> pixels(IWICBitmapSource* source) {
        ComPtr<IWICFormatConverter> convert;
        if (FAILED(factory_->CreateFormatConverter(&convert)) ||
            FAILED(convert->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
        auto icon = std::make_shared<AppIcon>();
        if (FAILED(convert->CopyPixels(nullptr, AppIcon::size * 4, static_cast<UINT>(icon->pixels.size()),
            reinterpret_cast<BYTE*>(icon->pixels.data())))) return {};
        return icon;
    }
    ComPtr<IWICImagingFactory> factory_;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path) : path_(path) {
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        const auto name = utf8(path.c_str());
        if (sqlite3_open_v2(name.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) { close(); return; }
        sqlite3_busy_timeout(db_, 1000);
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 65536);
        // Bound the RAM page cache, not the on-disk archive or its row count.
        const char* schema = "PRAGMA cache_size=-128;"
            "PRAGMA mmap_size=0; PRAGMA auto_vacuum=INCREMENTAL; PRAGMA journal_mode=WAL; PRAGMA temp_store=MEMORY;"
            "CREATE TABLE IF NOT EXISTS icons(exe TEXT PRIMARY KEY,stamp INTEGER NOT NULL,bytes INTEGER NOT NULL,png BLOB NOT NULL,used INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS icons_exe_nocase ON icons(exe COLLATE NOCASE);";
        if (sqlite3_exec(db_, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
            diagnostics::write(diagnostics::Level::warning,"icons.database","cannot initialize shared icons table");
            close();
        } else diagnostics::write(diagnostics::Level::info,"icons.database","opened shared SQLite icons table");
    }
    ~Database() { close(); }
    bool available() const { return db_ != nullptr; }
    std::shared_ptr<AppIcon> get(const std::wstring& path,std::uint64_t stamp,std::uint64_t size,bool metadata,Codec& codec) {
        if (!db_) return {};
        Statement query;
        if (sqlite3_prepare_v2(db_,
            "SELECT png FROM icons WHERE exe=?1 COLLATE NOCASE AND (?4=0 OR (stamp=?2 AND bytes=?3)) ORDER BY used DESC LIMIT 1",
            -1,&query.value,nullptr)!=SQLITE_OK)return {};
        bind(query.value, path, stamp, size);
        sqlite3_bind_int(query.value,4,metadata?1:0);
        if (sqlite3_step(query.value) != SQLITE_ROW) return {};
        diagnostics::write(diagnostics::Level::trace,"icons.cache","persistent icon hit");
        return codec.decode(sqlite3_column_blob(query.value, 0), sqlite3_column_bytes(query.value, 0));
    }
    void put(const std::wstring& path, std::uint64_t stamp, std::uint64_t size, const std::vector<std::byte>& png) {
        if (!db_ || png.empty() || png.size() > 16384) return;
        // The icons table shares the 50 GiB emergency budget with history.
        std::error_code ec;
        const auto file_size=[&](const std::filesystem::path& file)->std::uint64_t {
            const auto n=std::filesystem::file_size(file,ec);
            if(ec){ec.clear();return 0;}return n;
        };
        auto wal=path_;wal+=L"-wal";
        auto shm=path_;shm+=L"-shm";
        if(file_size(path_)+file_size(wal)+file_size(shm)>=49ULL*1024*1024*1024 ||
            file_size(wal)>256ULL*1024*1024) {
            diagnostics::write(diagnostics::Level::warning,"icons.write","storage limit reached; icon persistence skipped");
            return;
        }
        // Keep older application paths available for future historical views.
        Statement insert;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO icons(exe,stamp,bytes,png,used) VALUES(?1,?2,?3,?4,?5)", -1, &insert.value, nullptr) != SQLITE_OK) return;
        bind(insert.value, path, stamp, size);
        sqlite3_bind_blob(insert.value, 4, png.data(), static_cast<int>(png.size()), SQLITE_TRANSIENT);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        sqlite3_bind_int64(insert.value, 5, now);
        const auto result=sqlite3_step(insert.value);
        if(result!=SQLITE_DONE) diagnostics::write(diagnostics::Level::warning,"icons.write",sqlite3_errmsg(db_));
        else diagnostics::write(diagnostics::Level::trace,"icons.write","persistent PNG stored");
    }
private:
    static void bind(sqlite3_stmt* statement, const std::wstring& path, std::uint64_t stamp, std::uint64_t size) {
        const auto name = utf8(path.c_str());
        sqlite3_bind_text(statement, 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(stamp));
        sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(size));
    }
    void close() { if (db_) { sqlite3_close(db_); db_ = nullptr; } }
    std::filesystem::path path_;
    sqlite3* db_{};
};
} // namespace

struct AppIconCache::Impl {
    struct Entry { std::shared_ptr<const AppIcon> image; std::uint64_t used{}; bool pending = true; };
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::map<std::wstring,Entry,std::less<>> entries;
    std::deque<std::wstring> queue;
    std::uint64_t clock{};
    std::size_t capacity = default_memory_limit;
    IconCacheStats counters;
    std::jthread thread;

    void request(std::wstring_view requested) {
        std::lock_guard lock(mutex);
        if(const auto found=entries.find(requested);found!=entries.end()) {found->second.used=++clock;return;}
        if(queue.size()>=capacity)return;
        trim(capacity-1);
        if(entries.size()>=capacity)return;
        const auto [entry,inserted]=entries.try_emplace(std::wstring(requested),Entry{{},++clock,true});
        if(inserted)queue.push_back(entry->first);
        wake.notify_all();
    }
    std::shared_ptr<const AppIcon> get(std::wstring_view requested) {
        std::lock_guard lock(mutex);
        const auto found=entries.find(requested);
        if(found==entries.end())return {};
        found->second.used=++clock;
        return found->second.image;
    }

    // Called only with mutex held. Never evict in-flight icon requests.
    void trim(std::size_t maximum) {
        while (entries.size() > maximum) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (!it->second.pending && (oldest == entries.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest == entries.end()) return;
            entries.erase(oldest);
        }
    }

    Impl(std::filesystem::path database, std::function<void()> ready) {
        thread = std::jthread([this, database = std::move(database), ready = std::move(ready)](std::stop_token stop) {
            const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            struct Uninit { bool active; ~Uninit() { if (active) CoUninitialize(); } } uninit{SUCCEEDED(com)};
            try {
                Codec codec;
                Database db(database);
                { std::lock_guard lock(mutex); counters.persistent = db.available(); }
                std::vector<std::byte> png;
                while (!stop.stop_requested()) {
                    std::wstring path;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait(lock, stop, [this] { return !queue.empty(); });
                        if (stop.stop_requested()) return;
                        path = std::move(queue.front()); queue.pop_front();
                    }
                    WIN32_FILE_ATTRIBUTE_DATA data{};
                    const bool metadata = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
                    const auto stamp = filetime_ticks(data.ftLastWriteTime);
                    const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                    auto image = !path.empty()?db.get(path,stamp,size,metadata,codec):nullptr;
                    const bool hit = image != nullptr;
                    if (!image) {
                        diagnostics::write(diagnostics::Level::trace,"icons.extract","cache miss, extracting executable icon");
                        png.clear(); image = codec.extract(path, png);
                        if (metadata) db.put(path, stamp, size, png);
                    }
                    {
                        std::lock_guard lock(mutex);
                        if (const auto found = entries.find(path); found != entries.end()) { found->second.image = std::move(image); found->second.pending = false; }
                        trim(capacity);
                        if (hit) ++counters.disk_hits; else ++counters.extractions;
                    }
                    ready();
                }
            } catch (...) { /* Icons are optional; collection and the client stay usable. */ }
        });
    }
    ~Impl() { thread.request_stop(); wake.notify_all(); if (thread.joinable()) thread.join(); }
};

AppIconCache::AppIconCache(std::filesystem::path path, std::function<void()> ready)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(ready))) {}
AppIconCache::~AppIconCache() = default;
void AppIconCache::set_capacity(std::size_t capacity) {
    std::lock_guard lock(impl_->mutex);
    impl_->capacity = std::clamp<std::size_t>(capacity,16,128);
    impl_->trim(impl_->capacity);
}
void AppIconCache::request(std::wstring_view path) {impl_->request(path);}
std::shared_ptr<const AppIcon> AppIconCache::get(std::wstring_view path) {return impl_->get(path);}
IconCacheStats AppIconCache::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto stats = impl_->counters; stats.entries = impl_->entries.size(); stats.capacity = impl_->capacity;
    for (const auto& [id, entry] : impl_->entries) { (void)id; if (entry.pending) ++stats.pending; }
    return stats;
}

HICON native_app_icon(const AppIcon& image) {
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(info.bmiHeader);
    info.bmiHeader.biWidth=AppIcon::size;
    info.bmiHeader.biHeight=-static_cast<LONG>(AppIcon::size);
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    void* pixels{};
    const auto color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!color)return nullptr;
    std::memcpy(pixels,image.pixels.data(),image.pixels.size());
    const std::array<std::byte,AppIcon::size*AppIcon::size/8> transparent{};
    const auto mask=CreateBitmap(AppIcon::size,AppIcon::size,1,1,transparent.data());
    ICONINFO icon{TRUE,0,0,mask,color};
    const auto result=mask?CreateIconIndirect(&icon):nullptr;
    if(mask)DeleteObject(mask);
    DeleteObject(color);
    return result;
}

bool save_argb_png(const std::filesystem::path& file, const void* pixels, unsigned width, unsigned height) {
    if (!pixels || !width || !height || width > 8192 || height > 8192) return false;
    std::error_code error; std::filesystem::create_directories(file.parent_path(), error);
    if (error) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppPBGRA,width*4,width*height*4,
            static_cast<BYTE*>(const_cast<void*>(pixels)),&bitmap)) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) ||
        FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame,&properties)) || FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(width,height))) return false;
    auto format=GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(),nullptr)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
} // namespace fluxtop
