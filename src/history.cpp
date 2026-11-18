#include "fluxtop/history.hpp"
#include "fluxtop/windows.hpp"
#include <winsqlite/winsqlite3.h>
#include <chrono>
#include <cmath>
#include <limits>
#include <type_traits>

namespace fluxtop {
namespace {
struct Statement {
    sqlite3_stmt* value{};
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void reset() { sqlite3_reset(value); sqlite3_clear_bindings(value); }
    void text(int index, const std::string& value_) {
        if (sqlite3_bind_text(value, index, value_.c_str(), static_cast<int>(value_.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("History text binding failed");
    }
    void integer(int index, std::uint64_t number) {
        if (number > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())
            || sqlite3_bind_int64(value, index, static_cast<sqlite3_int64>(number)) != SQLITE_OK)
            throw std::runtime_error("History integer out of range");
    }
    template<class T> void optional(int index, std::optional<T> number) {
        if (!number) return;
        if constexpr (std::is_floating_point_v<T>) {
            if (std::isfinite(*number) && sqlite3_bind_double(value, index, *number) != SQLITE_OK)
                throw std::runtime_error("History numeric binding failed");
        } else integer(index, *number);
    }
    int step() {
        const auto result = sqlite3_step(value);
        if (result != SQLITE_DONE && result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(value)));
        return result;
    }
};
void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
std::wstring environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) return {};
    value.resize(count); return value;
}
}
std::filesystem::path history_path() {
    if (auto path = environment(L"FLUXTOP_HISTORY_DB"); !path.empty()) return std::filesystem::absolute(path);
    const auto base = environment(L"LOCALAPPDATA");
    return base.empty() ? std::filesystem::path{} : std::filesystem::path(base) / L"FluxTop" / L"history.db";
}
struct HistoryWriter::Impl {
    std::filesystem::path path;
    sqlite3* db{};
    std::uint64_t count{}, sequence{};
    std::string session;
    struct ProcessId { std::optional<std::uint64_t> creation; sqlite3_int64 id; };
    std::unordered_map<std::uint32_t, ProcessId> live;
    std::unique_ptr<Statement> app_insert, app_select, process_insert, process_select, sample_insert, measurement_insert, counter, score_insert;
    SpikeTracker score_tracker;
    explicit Impl(std::filesystem::path file) : path(std::move(file)) {
        LARGE_INTEGER ticks{}; QueryPerformanceCounter(&ticks);
        session = std::to_string(GetCurrentProcessId()) + ":" + std::to_string(ticks.QuadPart);
    }
    void close() noexcept {
        app_insert.reset(); app_select.reset(); process_insert.reset(); process_select.reset();
        sample_insert.reset(); measurement_insert.reset(); counter.reset(); score_insert.reset();
        live.clear(); if (db) sqlite3_close(db); db = nullptr;
    }
    ~Impl() { close(); }
    void open() {
        if (db) return;
        if (path.empty()) throw std::runtime_error("History database path unavailable");
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        const auto name = utf8(path.c_str());
        if (sqlite3_open_v2(name.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error(db ? sqlite3_errmsg(db) : "Cannot open history database");
        sqlite3_busy_timeout(db, 100);
        execute(db, "PRAGMA foreign_keys=ON; PRAGMA cache_size=-256; PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;"
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=64;");
        Statement version(db, "PRAGMA user_version"); version.step();
        const auto schema = sqlite3_column_int(version.value, 0);
        if (schema != 0 && schema != 1) throw std::runtime_error("Unsupported history schema version");
        if (schema == 0) execute(db,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE applications(id INTEGER PRIMARY KEY, identity TEXT NOT NULL UNIQUE, name TEXT NOT NULL, exe_path TEXT);"
            "CREATE TABLE process_instances(id INTEGER PRIMARY KEY, identity TEXT NOT NULL UNIQUE, application_id INTEGER NOT NULL REFERENCES applications(id),"
            "pid INTEGER NOT NULL, creation_100ns INTEGER, first_seen_ms INTEGER NOT NULL);"
            "CREATE INDEX instances_application ON process_instances(application_id);"
            "CREATE TABLE snapshots(id INTEGER PRIMARY KEY, timestamp_ms INTEGER NOT NULL, interval_ms REAL NOT NULL,"
            "cpu_percent REAL, ram_used_bytes INTEGER, ram_capacity_bytes INTEGER, gpu_percent REAL, gpu_memory_bytes INTEGER,"
            "rx_bytes_per_second REAL, tx_bytes_per_second REAL, network_status TEXT, gpu_status TEXT);"
            "CREATE INDEX snapshots_time ON snapshots(timestamp_ms);"
            "CREATE TABLE measurements(sample_id INTEGER NOT NULL REFERENCES snapshots(id), process_instance_id INTEGER NOT NULL REFERENCES process_instances(id),"
            "cpu_percent REAL, working_set_bytes INTEGER, private_bytes INTEGER, gpu_percent REAL, gpu_memory_bytes INTEGER,"
            "rx_bytes_per_second REAL, tx_bytes_per_second REAL, PRIMARY KEY(sample_id,process_instance_id)) WITHOUT ROWID;"
            "CREATE INDEX measurements_process_time ON measurements(process_instance_id,sample_id);"
            "CREATE TABLE history_state(id INTEGER PRIMARY KEY CHECK(id=1), measurement_count INTEGER NOT NULL);"
            "INSERT INTO history_state VALUES(1,0); PRAGMA user_version=1; COMMIT;");
        // Add an optional score table while keeping user_version=1 for rollback
        // compatibility with published 0.2.2 clients (they ignore this table).
        execute(db,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS application_scores("
            "sample_id INTEGER NOT NULL REFERENCES snapshots(id),"
            "application_id INTEGER NOT NULL REFERENCES applications(id),"
            "score REAL NOT NULL, PRIMARY KEY(sample_id,application_id)) WITHOUT ROWID;"
            "CREATE INDEX IF NOT EXISTS application_scores_app ON application_scores(application_id,sample_id);"
            "COMMIT;");
        Statement state(db, "SELECT measurement_count FROM history_state WHERE id=1");
        if (state.step() != SQLITE_ROW) throw std::runtime_error("History record counter missing");
        count = static_cast<std::uint64_t>(sqlite3_column_int64(state.value, 0));
        app_insert = std::make_unique<Statement>(db, "INSERT OR IGNORE INTO applications(identity,name,exe_path) VALUES(?,?,?)");
        app_select = std::make_unique<Statement>(db, "SELECT id FROM applications WHERE identity=?");
        process_insert = std::make_unique<Statement>(db, "INSERT OR IGNORE INTO process_instances(identity,application_id,pid,creation_100ns,first_seen_ms) VALUES(?,?,?,?,?)");
        process_select = std::make_unique<Statement>(db, "SELECT id FROM process_instances WHERE identity=?");
        sample_insert = std::make_unique<Statement>(db, "INSERT INTO snapshots(timestamp_ms,interval_ms,cpu_percent,ram_used_bytes,ram_capacity_bytes,gpu_percent,gpu_memory_bytes,rx_bytes_per_second,tx_bytes_per_second,network_status,gpu_status) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        measurement_insert = std::make_unique<Statement>(db, "INSERT INTO measurements VALUES(?,?,?,?,?,?,?,?,?)");
        counter = std::make_unique<Statement>(db, "UPDATE history_state SET measurement_count=measurement_count+? WHERE id=1");
        score_insert = std::make_unique<Statement>(db,
            "INSERT OR REPLACE INTO application_scores(sample_id,application_id,score) VALUES(?,?,?)");
    }
    sqlite3_int64 process(const Process& row, std::int64_t timestamp) {
        if (const auto found = live.find(row.pid); found != live.end() && found->second.creation == row.creation_time)
            return found->second.id;
        const auto identity = row.exe_path ? "path:" + *row.exe_path : "name:" + row.name;
        auto& insert = *app_insert; insert.reset(); insert.text(1, identity); insert.text(2, row.name);
        if (row.exe_path) insert.text(3, *row.exe_path); insert.step();
        auto& select = *app_select; select.reset(); select.text(1, identity);
        if (select.step() != SQLITE_ROW) throw std::runtime_error("History application missing");
        const auto application = sqlite3_column_int64(select.value, 0);
        const auto instance = row.creation_time ? std::to_string(row.pid) + ":" + std::to_string(*row.creation_time)
            : session + ":unknown:" + std::to_string(row.pid) + ":" + std::to_string(++sequence);
        auto& process = *process_insert; process.reset(); process.text(1, instance);
        process.integer(2, static_cast<std::uint64_t>(application)); process.integer(3, row.pid);
        process.optional(4, row.creation_time); process.integer(5, static_cast<std::uint64_t>(timestamp)); process.step();
        auto& lookup = *process_select; lookup.reset(); lookup.text(1, instance);
        if (lookup.step() != SQLITE_ROW) throw std::runtime_error("History process missing");
        return sqlite3_column_int64(lookup.value, 0);
    }
    void append(const Snapshot& snapshot) {
        execute(db, "BEGIN IMMEDIATE");
        // Identical aggregate + AutoRank heuristic as the live Auto overlay.
        auto groups=aggregate_application_processes(snapshot.processes);
        score_tracker.update(groups,snapshot.total_ram_capacity,snapshot.total_gpu_memory_capacity);
        auto& sample = *sample_insert; sample.reset();
        sample.integer(1, static_cast<std::uint64_t>(snapshot.timestamp_ms)); sample.optional(2, std::optional(snapshot.interval_ms));
        sample.optional(3, snapshot.total_cpu_percent); sample.optional(4, snapshot.total_ram_used); sample.optional(5, snapshot.total_ram_capacity);
        sample.optional(6, snapshot.total_gpu_percent); sample.optional(7, snapshot.total_gpu_memory);
        sample.optional(8, snapshot.total_rx_bytes_per_second); sample.optional(9, snapshot.total_tx_bytes_per_second);
        sample.text(10, snapshot.network_status); sample.text(11, snapshot.gpu_status); sample.step();
        const auto sample_id = sqlite3_last_insert_rowid(db);
        std::unordered_map<std::uint32_t, ProcessId> next;
        next.reserve(snapshot.processes.size());
        for (const auto& row : snapshot.processes) {
            const auto instance = process(row, snapshot.timestamp_ms);
            auto& insert = *measurement_insert; insert.reset();
            insert.integer(1, static_cast<std::uint64_t>(sample_id)); insert.integer(2, static_cast<std::uint64_t>(instance));
            insert.optional(3, row.cpu_percent); insert.optional(4, row.working_set); insert.optional(5, row.private_bytes);
            insert.optional(6, row.gpu_percent); insert.optional(7, row.dedicated_gpu_bytes);
            insert.optional(8, row.rx_bytes_per_second); insert.optional(9, row.tx_bytes_per_second); insert.step();
            next.emplace(row.pid, ProcessId{row.creation_time, instance});
        }
        for (const auto& group: groups) {
            const auto identity=group.exe_path ? "path:"+*group.exe_path : "name:"+group.name;
            auto& lookup=*app_select;lookup.reset();lookup.text(1,identity);
            if (lookup.step()!=SQLITE_ROW) throw std::runtime_error("AutoScore application missing");
            const auto app_id=sqlite3_column_int64(lookup.value,0);
            auto& score=*score_insert;score.reset();
            score.integer(1,static_cast<std::uint64_t>(sample_id));
            score.integer(2,static_cast<std::uint64_t>(app_id));
            if(sqlite3_bind_double(score.value,3,auto_rank(group,31U).score)!=SQLITE_OK)
                throw std::runtime_error("Cannot bind AutoScore");
            score.step();
        }
        counter->reset(); counter->integer(1, snapshot.processes.size()); counter->step();
        // Read the transactional counter so simultaneous collectors cannot lose increments.
        Statement state(db, "SELECT measurement_count FROM history_state WHERE id=1"); state.step();
        const auto saved = static_cast<std::uint64_t>(sqlite3_column_int64(state.value, 0));
        execute(db, "COMMIT"); count = saved; live = std::move(next);
    }
};
HistoryWriter::HistoryWriter(std::filesystem::path path) : impl_(std::make_unique<Impl>(std::move(path))) {}
HistoryWriter::~HistoryWriter() = default;
void HistoryWriter::append(Snapshot& snapshot) {
    const auto start = std::chrono::steady_clock::now();
    try {
        impl_->open(); impl_->append(snapshot); snapshot.history_records = impl_->count; snapshot.history_error.clear();
    } catch (const std::exception& error) {
        snapshot.history_error = error.what();
        // Closing rolls back the entire failed interval, including the counter.
        impl_->close();
    }
    snapshot.history_write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}
