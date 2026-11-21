#include "fluxtop/history.hpp"

#include "fluxtop/diagnostics.hpp"
#include "fluxtop/history_math.hpp"
#include <winsqlite/winsqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fluxtop {
namespace {
constexpr std::uint64_t mib=1024ULL*1024;
constexpr std::size_t max_pending_minutes=8;
constexpr std::size_t max_pending_bytes=2*1024*1024;

std::string database_filename(const std::filesystem::path& p) {
    const auto u=p.u8string();return {reinterpret_cast<const char*>(u.data()),u.size()};
}
void run_sql(sqlite3* db,const char* sql) {
    if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
struct Query {
    sqlite3_stmt* s{};
    Query(sqlite3* db,const char* sql) {
        if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query(){sqlite3_finalize(s);}
    Query(const Query&)=delete;
    Query& operator=(const Query&)=delete;
    void reset(){sqlite3_reset(s);sqlite3_clear_bindings(s);}
    void integer(int i,std::int64_t v){if(sqlite3_bind_int64(s,i,v)!=SQLITE_OK)throw std::runtime_error("sqlite integer binding");}
    void real(int i,double v){if(sqlite3_bind_double(s,i,v)!=SQLITE_OK)throw std::runtime_error("sqlite real binding");}
    void text(int i,const std::string& v){if(sqlite3_bind_text(s,i,v.c_str(),static_cast<int>(v.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("sqlite text binding");}
    void done(){if(sqlite3_step(s)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s)));}
};
struct Transaction {
    sqlite3* db{};
    explicit Transaction(sqlite3* d):db(d){run_sql(db,"BEGIN IMMEDIATE");}
    ~Transaction(){if(db)sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}
    void commit(){run_sql(db,"COMMIT");db=nullptr;}
};
std::uint64_t file_bytes(const std::filesystem::path& path){
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    return error?0:size;
}
std::uint64_t storage_bytes(const std::filesystem::path& path){
    return file_bytes(path)+file_bytes(std::filesystem::path(path.wstring()+L"-wal"))+
           file_bytes(std::filesystem::path(path.wstring()+L"-shm"));
}
std::uint64_t wal_bytes(const std::filesystem::path& path){
    return file_bytes(std::filesystem::path(path.wstring()+L"-wal"));
}
constexpr char create_sql[] =
    "CREATE TABLE applications(id INTEGER PRIMARY KEY,identity TEXT NOT NULL UNIQUE,name TEXT NOT NULL,exe_path TEXT);"
    "CREATE TABLE history_rollups("
    "level INTEGER NOT NULL,bucket_ms INTEGER NOT NULL,application_id INTEGER NOT NULL REFERENCES applications(id),"
    "uptime_ms INTEGER NOT NULL,last_seen_ms INTEGER NOT NULL,samples INTEGER NOT NULL,"
    "score_integral REAL NOT NULL,score_duration_ms INTEGER NOT NULL,score_min REAL,score_max REAL,"
    "cpu_integral REAL NOT NULL,cpu_duration_ms INTEGER NOT NULL,"
    "ram_integral REAL NOT NULL,ram_duration_ms INTEGER NOT NULL,"
    "gpu_integral REAL NOT NULL,gpu_duration_ms INTEGER NOT NULL,"
    "vram_integral REAL NOT NULL,vram_duration_ms INTEGER NOT NULL,"
    "rx_integral REAL NOT NULL,rx_duration_ms INTEGER NOT NULL,"
    "tx_integral REAL NOT NULL,tx_duration_ms INTEGER NOT NULL,"
    "PRIMARY KEY(level,bucket_ms,application_id)) WITHOUT ROWID;"
    "CREATE INDEX rollups_app_time ON history_rollups(application_id,bucket_ms);"
    "CREATE INDEX rollups_time ON history_rollups(bucket_ms);"
    "CREATE TABLE IF NOT EXISTS icons(exe TEXT PRIMARY KEY,stamp INTEGER NOT NULL,bytes INTEGER NOT NULL,png BLOB NOT NULL,used INTEGER NOT NULL);"
    "CREATE TABLE history_state(id INTEGER PRIMARY KEY CHECK(id=1),first_ms INTEGER,last_ms INTEGER,observations INTEGER NOT NULL DEFAULT 0);"
    "INSERT INTO history_state(id,observations) VALUES(1,0);"
    "PRAGMA user_version=2;";

// A single deterministic upsert is used for all tiers, ensuring replay-safe
// arithmetic when a process crashes between closing a minute and a checkpoint.
constexpr char upsert_sql[] =
    "INSERT INTO history_rollups VALUES("
    "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22) "
    "ON CONFLICT(level,bucket_ms,application_id) DO UPDATE SET "
    "uptime_ms=uptime_ms+excluded.uptime_ms,"
    "last_seen_ms=MAX(last_seen_ms,excluded.last_seen_ms),"
    "samples=samples+excluded.samples,"
    "score_integral=score_integral+excluded.score_integral,"
    "score_duration_ms=score_duration_ms+excluded.score_duration_ms,"
    "score_min=MIN(score_min,excluded.score_min),score_max=MAX(score_max,excluded.score_max),"
    "cpu_integral=cpu_integral+excluded.cpu_integral,cpu_duration_ms=cpu_duration_ms+excluded.cpu_duration_ms,"
    "ram_integral=ram_integral+excluded.ram_integral,ram_duration_ms=ram_duration_ms+excluded.ram_duration_ms,"
    "gpu_integral=gpu_integral+excluded.gpu_integral,gpu_duration_ms=gpu_duration_ms+excluded.gpu_duration_ms,"
    "vram_integral=vram_integral+excluded.vram_integral,vram_duration_ms=vram_duration_ms+excluded.vram_duration_ms,"
    "rx_integral=rx_integral+excluded.rx_integral,rx_duration_ms=rx_duration_ms+excluded.rx_duration_ms,"
    "tx_integral=tx_integral+excluded.tx_integral,tx_duration_ms=tx_duration_ms+excluded.tx_duration_ms";

struct ApplicationMinute {
    std::string identity,name,path;
    HistoryAccumulator stats;
};
struct MinuteBatch {
    std::int64_t bucket_ms{};
    std::unordered_map<std::string,ApplicationMinute> apps;
};
std::size_t footprint(const MinuteBatch& batch){
    std::size_t size=sizeof(batch);
    for(const auto& [identity,app]:batch.apps)
        size+=sizeof(app)+identity.capacity()+app.identity.capacity()+app.name.capacity()+app.path.capacity()+128;
    return size;
}
struct Observation {
    std::string identity,name,path;
    std::array<std::optional<double>,history_metric_count> metrics{};
};
std::array<std::optional<double>,history_metric_count> metrics_of(const Process& group){
    std::array<std::optional<double>,history_metric_count> v{};
    v[0]=auto_rank(group,63U).score; // Physical-disk score appears only with trustworthy ETW data.
    v[1]=group.cpu_percent;
    if(group.working_set) v[2]=static_cast<double>(*group.working_set);
    v[3]=group.gpu_percent;
    if(group.dedicated_gpu_bytes) v[4]=static_cast<double>(*group.dedicated_gpu_bytes);
    v[5]=group.rx_bytes_per_second;
    v[6]=group.tx_bytes_per_second;
    return v;
}
void bind_rollup(Query& q,int level,std::int64_t bucket,std::int64_t app_id,const HistoryAccumulator& a){
    q.integer(1,level);q.integer(2,bucket);q.integer(3,app_id);
    q.integer(4,a.uptime_ms);q.integer(5,a.last_seen_ms);
    q.integer(6,static_cast<std::int64_t>(a.values[0].samples));
    q.real(7,a.values[0].numerator());q.integer(8,a.values[0].duration_ms);
    if(a.values[0].duration_ms){q.real(9,a.values[0].minimum);q.real(10,a.values[0].maximum);}
    for(int i=1;i<static_cast<int>(history_metric_count);++i){
        q.real(9+i*2,a.values[i].numerator());q.integer(10+i*2,a.values[i].duration_ms);
    }
}
// Compaction operates only on one completed clock bucket, never scans all history.
// Every UPSERT and DELETE share a transaction: interruption cannot double-count.
bool compact_one(sqlite3* db,int source,int target,std::int64_t target_width,std::int64_t completed_before){
    Query oldest(db,"SELECT MIN(bucket_ms) FROM history_rollups WHERE level=?1 AND bucket_ms<?2");
    oldest.integer(1,source);oldest.integer(2,completed_before);
    if(sqlite3_step(oldest.s)!=SQLITE_ROW || sqlite3_column_type(oldest.s,0)==SQLITE_NULL)return false;
    const auto first=sqlite3_column_int64(oldest.s,0);
    sqlite3_reset(oldest.s); // Release the reader before write lock.
    const auto start=history_floor(first,target_width);
    const auto end=start+target_width;
    if(end>completed_before) return false; // Incomplete hour/day.
    Transaction tx(db);
    {
        Query merge(db,
            "INSERT INTO history_rollups "
            "SELECT ?1,?2,application_id,SUM(uptime_ms),MAX(last_seen_ms),SUM(samples),"
            "SUM(score_integral),SUM(score_duration_ms),MIN(score_min),MAX(score_max),"
            "SUM(cpu_integral),SUM(cpu_duration_ms),SUM(ram_integral),SUM(ram_duration_ms),"
            "SUM(gpu_integral),SUM(gpu_duration_ms),SUM(vram_integral),SUM(vram_duration_ms),"
            "SUM(rx_integral),SUM(rx_duration_ms),SUM(tx_integral),SUM(tx_duration_ms) "
            "FROM history_rollups WHERE level=?3 AND bucket_ms>=?2 AND bucket_ms<?4 GROUP BY application_id "
            "ON CONFLICT(level,bucket_ms,application_id) DO UPDATE SET "
            "uptime_ms=uptime_ms+excluded.uptime_ms,last_seen_ms=MAX(last_seen_ms,excluded.last_seen_ms),"
            "samples=samples+excluded.samples,score_integral=score_integral+excluded.score_integral,"
            "score_duration_ms=score_duration_ms+excluded.score_duration_ms,"
            "score_min=MIN(score_min,excluded.score_min),score_max=MAX(score_max,excluded.score_max),"
            "cpu_integral=cpu_integral+excluded.cpu_integral,cpu_duration_ms=cpu_duration_ms+excluded.cpu_duration_ms,"
            "ram_integral=ram_integral+excluded.ram_integral,ram_duration_ms=ram_duration_ms+excluded.ram_duration_ms,"
            "gpu_integral=gpu_integral+excluded.gpu_integral,gpu_duration_ms=gpu_duration_ms+excluded.gpu_duration_ms,"
            "vram_integral=vram_integral+excluded.vram_integral,vram_duration_ms=vram_duration_ms+excluded.vram_duration_ms,"
            "rx_integral=rx_integral+excluded.rx_integral,rx_duration_ms=rx_duration_ms+excluded.rx_duration_ms,"
            "tx_integral=tx_integral+excluded.tx_integral,tx_duration_ms=tx_duration_ms+excluded.tx_duration_ms");
        merge.integer(1,target);merge.integer(2,start);merge.integer(3,source);merge.integer(4,end);merge.done();
    }
    {
        Query del(db,"DELETE FROM history_rollups WHERE level=?1 AND bucket_ms>=?2 AND bucket_ms<?3");
        del.integer(1,source);del.integer(2,start);del.integer(3,end);del.done();
    }
    tx.commit();
    diagnostics::write(diagnostics::Level::trace,"history.rollup",
        "source="+std::to_string(source)+" target="+std::to_string(target)+" bucket="+std::to_string(start));
    return true;
}
void compact(sqlite3* db,std::int64_t current_minute){
    const auto hour_start=history_floor(current_minute,history_hour_ms);
    const auto day_start=history_floor(current_minute,history_day_ms);
    // Bound expensive maintenance to a few completed time buckets per minute.
    // If the user was away for months, subsequent minute ticks catch up incrementally.
    for(int i=0;i<4 && compact_one(db,0,1,history_hour_ms,hour_start);++i){}
    for(int i=0;i<4 && compact_one(db,1,2,history_day_ms,day_start);++i){}
    const auto expires=current_minute-history_retention_days*history_day_ms;
    Query old(db,"SELECT MIN(bucket_ms) FROM history_rollups WHERE level=2 AND bucket_ms<?1");
    old.integer(1,expires);
    const bool has_expired=sqlite3_step(old.s)==SQLITE_ROW && sqlite3_column_type(old.s,0)!=SQLITE_NULL;
    sqlite3_reset(old.s);
    if(has_expired){
        Transaction tx(db);
        Query expire(db,"DELETE FROM history_rollups WHERE level=2 AND bucket_ms IN ("
            "SELECT bucket_ms FROM history_rollups WHERE level=2 AND bucket_ms<?1 LIMIT 1)");
        expire.integer(1,expires);expire.done();
        run_sql(db,"UPDATE history_state SET first_ms=(SELECT MIN(bucket_ms) FROM history_rollups) WHERE id=1");
        tx.commit();
    }
}
void shrink_wal(sqlite3* db,const std::filesystem::path& path){
    const auto wal=wal_bytes(path);
    int total{},checkpointed{};
    (void)sqlite3_wal_checkpoint_v2(db,nullptr,SQLITE_CHECKPOINT_PASSIVE,&total,&checkpointed);
    if(wal>64*mib){
        // SQLite may return BUSY while a reader is active. In that case the writer
        // will stop before the WAL can grow without limit.
        (void)sqlite3_wal_checkpoint_v2(db,nullptr,SQLITE_CHECKPOINT_TRUNCATE,&total,&checkpointed);
    }
    if(wal>64*mib) diagnostics::write(diagnostics::Level::warning,"history.checkpoint",
        "wal_before="+std::to_string(wal)+" wal_after="+std::to_string(wal_bytes(path)));
    if(wal_bytes(path)>256*mib) throw std::runtime_error("History WAL exceeds 256 MiB: checkpoint blocked; recording paused");
}
void init_db(sqlite3* db){
    run_sql(db,"PRAGMA foreign_keys=ON;PRAGMA busy_timeout=1000;PRAGMA cache_size=-1024;"
               "PRAGMA temp_store=MEMORY;PRAGMA mmap_size=0;");
    Query version(db,"PRAGMA user_version");
    if(sqlite3_step(version.s)!=SQLITE_ROW)throw std::runtime_error("Cannot read SQLite schema");
    const int schema=sqlite3_column_int(version.s,0);
    sqlite3_reset(version.s);
    if(schema==0){
        run_sql(db,"PRAGMA auto_vacuum=INCREMENTAL;");
        run_sql(db,"PRAGMA journal_mode=WAL;");
        Transaction tx(db);run_sql(db,create_sql);tx.commit();
    }else if(schema!=2){
        throw std::runtime_error("Existing history schema is incompatible with rollups; remove old history.db/-wal/-shm while FluxTop is stopped");
    }else run_sql(db,"PRAGMA journal_mode=WAL;");
    run_sql(db,"PRAGMA synchronous=FULL;PRAGMA wal_autocheckpoint=0;PRAGMA journal_size_limit=67108864;");
}
void write_minute(sqlite3* db,const std::filesystem::path& path,const MinuteBatch& batch){
    if(storage_bytes(path)>=history_stop_bytes)
        throw std::runtime_error("History storage reached 49 GiB safety threshold (50 GiB emergency limit); recording paused");
    if(wal_bytes(path)>256*mib) {
        shrink_wal(db,path);
    }
    Transaction tx(db);
    {
        Query insert_app(db,"INSERT OR IGNORE INTO applications(identity,name,exe_path) VALUES(?1,?2,?3)");
        Query select_app(db,"SELECT id FROM applications WHERE identity=?1");
        Query insert_rollup(db,upsert_sql);
        for(const auto& [key,app]:batch.apps){
            insert_app.reset();insert_app.text(1,key);insert_app.text(2,app.name);
            if(!app.path.empty())insert_app.text(3,app.path);
            insert_app.done();
            select_app.reset();select_app.text(1,key);
            if(sqlite3_step(select_app.s)!=SQLITE_ROW)throw std::runtime_error("Cannot look up history application");
            const auto id=sqlite3_column_int64(select_app.s,0);
            sqlite3_reset(select_app.s);
            insert_rollup.reset();bind_rollup(insert_rollup,0,batch.bucket_ms,id,app.stats);insert_rollup.done();
        }
    }
    {
        Query state(db,"UPDATE history_state SET "
            "first_ms=MIN(COALESCE(first_ms,?1),?1),last_ms=MAX(COALESCE(last_ms,?2),?2),"
            "observations=observations+?3 WHERE id=1");
        std::int64_t count=0,last_seen=batch.bucket_ms;
        for(const auto& a:batch.apps){
            count+=static_cast<std::int64_t>(a.second.stats.values[0].samples);
            last_seen=std::max(last_seen,a.second.stats.last_seen_ms);
        }
        state.integer(1,batch.bucket_ms);state.integer(2,last_seen);
        state.integer(3,count);state.done();
    }
    tx.commit();
    // Closing and maintenance are separate bounded SQLite transactions, on the
    // background thread only. Neither blocks CPU/GPU/network sampling.
    compact(db,batch.bucket_ms+history_minute_ms);
    shrink_wal(db,path);
    if(storage_bytes(path)>=history_emergency_bytes)
        throw std::runtime_error("History exceeds 50 GiB emergency cap; recording disabled");
    run_sql(db,"PRAGMA incremental_vacuum(32)");
}
std::wstring environment_w(const wchar_t* key){
#ifdef _WIN32
    DWORD size=GetEnvironmentVariableW(key,nullptr,0);
    if(!size || size>32768)return {};
    std::wstring out(size,L'\0');
    const auto written=GetEnvironmentVariableW(key,out.data(),size);
    if(!written || written>=size)return {};
    out.resize(written);return out;
#else
    (void)key;return {};
#endif
}
}
std::filesystem::path history_path(){
    if(auto custom=environment_w(L"FLUXTOP_DATA_DB");!custom.empty())return std::filesystem::absolute(custom);
    if(auto base=environment_w(L"LOCALAPPDATA");!base.empty())return std::filesystem::path(base)/L"FluxTop"/L"fluxtop.db";
#ifndef _WIN32
    if(const char* v=std::getenv("FLUXTOP_HISTORY_DB"))return std::filesystem::absolute(v);
    return std::filesystem::temp_directory_path()/"fluxtop-history.db";
#else
    return {};
#endif
}

struct HistoryWriter::Impl {
    std::filesystem::path path;
    SpikeTracker scores;
    MinuteBatch current;
    bool has_current{};
    std::int64_t last_end_ms{};
    std::deque<MinuteBatch> queue;
    std::size_t queue_bytes{};
    std::mutex mutex;
    std::condition_variable ready;
    std::thread thread;
    bool closing{},halted{};
    std::string error;
    std::atomic<std::uint64_t> saved{0};
    explicit Impl(std::filesystem::path p):path(std::move(p)){
        thread=std::thread([this]{run();});
    }
    ~Impl(){
        if(has_current)push(std::move(current));
        {std::lock_guard lock(mutex);closing=true;}
        ready.notify_one();
        if(thread.joinable())thread.join();
    }
    void publish_error(const std::string& message){
        std::lock_guard lock(mutex);error=message;
    }
    void push(MinuteBatch batch){
        if(batch.apps.empty())return;
        const auto bytes=footprint(batch);
        {
            std::lock_guard lock(mutex);
            if(halted)return; // Keep the original error; never grow it every minute.
            if(queue.size()>=max_pending_minutes || queue_bytes+bytes>max_pending_bytes){
                diagnostics::write(diagnostics::Level::error,"history.backpressure","writer queue capacity exceeded");
                halted=true;queue.clear();queue_bytes=0;
                error="History queue overflow; recording stopped instead of silently losing minutes";
                return;
            }
            queue_bytes+=bytes;
            queue.push_back(std::move(batch));
        }
        ready.notify_one();
    }
    void add_bucket(std::int64_t bucket,std::int64_t duration,std::int64_t timestamp,
                    const std::vector<Observation>& rows){
        if(!has_current){current.bucket_ms=bucket;has_current=true;}
        if(bucket!=current.bucket_ms){
            push(std::move(current));
            current=MinuteBatch{};current.bucket_ms=bucket;
        }
        for(const auto& row:rows){
            if(current.apps.size()>=4'096 && !current.apps.contains(row.identity)){
                publish_error("Too many unique applications in one minute; excess rows cannot be recorded");
                continue;
            }
            auto [it,created]=current.apps.try_emplace(row.identity);
            auto& a=it->second;
            if(created){a.identity=row.identity;a.name=row.name;a.path=row.path;}
            a.stats.observe(row.metrics,duration,timestamp);
        }
    }
    void append(Snapshot& snapshot){
        const auto started=std::chrono::steady_clock::now();
        // Same AutoScore code as the existing overlay/history. No CPU/RAM/GPU
        // collection code or raw measurements are altered.
        auto groups=aggregate_application_processes(snapshot.processes);
        // Per-PID GPU memory was validated before grouping. Retain verified
        // member sums; do not compare shared-process totals with adapter usage.
        scores.update(groups,snapshot.total_ram_capacity,snapshot.total_gpu_memory_capacity);
        std::vector<Observation> observations;observations.reserve(groups.size());
        for(const auto& group:groups){
            Observation o;
            o.identity=group.exe_path?"path:"+*group.exe_path:"name:"+group.name;
            o.name=group.name;o.path=group.exe_path.value_or("");
            o.metrics=metrics_of(group);
            observations.push_back(std::move(o));
        }
        const auto end=snapshot.timestamp_ms;
        const auto period=std::isfinite(snapshot.interval_ms)
            ? static_cast<std::int64_t>(std::clamp(snapshot.interval_ms,0.0,10000.0)) : 0;
        const auto begin=std::max(end-period,last_end_ms);
        if(end>last_end_ms && begin<end){
            std::int64_t t=begin;
            while(t<end){
                const auto bucket=history_floor(t,history_minute_ms);
                const auto next=std::min(end,bucket+history_minute_ms);
                add_bucket(bucket,next-t,next,observations);
                t=next;
            }
            last_end_ms=end;
        }
        snapshot.history_records=saved.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex);
            snapshot.history_error=error;
        }
        snapshot.history_write_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    }
    void run()noexcept {
        sqlite3* db=nullptr;
        try {
            // Delay opening SQLite until there is a completed minute; avoids an
            // idle DB connection and all disk writes on the sampling path.
            for(;;){
                MinuteBatch batch;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock,[this]{return closing || !queue.empty();});
                    if(queue.empty() && closing)break;
                    batch=std::move(queue.front());queue.pop_front();
                    const auto bytes=footprint(batch);
                    queue_bytes=bytes>queue_bytes?0:queue_bytes-bytes;
                }
                try {
                    if(!db){
                        if(path.empty())throw std::runtime_error("History database path unavailable");
                        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
                        if(sqlite3_open_v2(database_filename(path).c_str(),&db,
                            SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK)
                            throw std::runtime_error("Cannot open history database");
                        init_db(db);
                        diagnostics::write(diagnostics::Level::info,"history.open","unified SQLite database ready (schema=2)");
                    }
                    write_minute(db,path,batch);
                    diagnostics::write(diagnostics::Level::info,"history.minute",
                        "bucket_ms="+std::to_string(batch.bucket_ms)+" apps="+std::to_string(batch.apps.size()));
                    saved.fetch_add(batch.apps.size(),std::memory_order_relaxed);
                    std::lock_guard lock(mutex);
                    if(!halted)error.clear();
                } catch(const std::exception& ex){
                    diagnostics::write(diagnostics::Level::error,"history.failure",ex.what());
                    if(db){sqlite3_close(db);db=nullptr;}
                    publish_error(ex.what());
                    const auto msg=std::string(ex.what());
                    if(msg.find("49 GiB")!=std::string::npos || msg.find("50 GiB")!=std::string::npos ||
                       msg.find("256 MiB")!=std::string::npos || msg.find("incompatible")!=std::string::npos){
                        std::lock_guard lock(mutex);halted=true;queue.clear();
                    }
                    // Failed writes are not silently retried: they may have
                    // committed before a post-commit checkpoint reported an error.
                }
            }
        }catch(const std::exception& e){publish_error(e.what());}
        if(db)sqlite3_close(db);
    }
};
HistoryWriter::HistoryWriter(std::filesystem::path path):impl_(std::make_unique<Impl>(std::move(path))){}
HistoryWriter::~HistoryWriter()=default;
void HistoryWriter::append(Snapshot& snapshot){impl_->append(snapshot);}
} // namespace fluxtop
