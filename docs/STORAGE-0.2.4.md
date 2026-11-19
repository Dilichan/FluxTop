# FluxTop 0.2.4: unified SQLite and activity journal

## Storage

The monitor uses one logical SQLite database:

`%LOCALAPPDATA%\FluxTop\fluxtop.db`

It contains the tables:

- `applications` — stable application identities;
- `history_rollups` — minute, hour and day time-weighted aggregates;
- `history_state` — persisted timestamps and total observation count;
- `icons` — cached PNG icons keyed by executable path and metadata.

The monitor and icon worker use separate SQLite connections to this **same** file.
All connections use WAL, because the statistics page reads while the background
writer is recording. SQLite may create `fluxtop.db-wal` and `fluxtop.db-shm`:
these are **transient support files**, not separate databases or duplicated tables.
Do not manually delete them while the application is running.

The 50 GiB emergency disk budget includes the main file, WAL and SHM. Icon
persistence follows that same budget. The asynchronous history queue is bounded
by both 8 minutes and an estimated 2 MiB. Each minute has an explicit unique-
application guard; a pathological cardinality limit is reported, not hidden.
The remaining in-progress minute is stored only as fixed-size per-application
accumulators, not as full snapshots.

No 0.2.3 migration is implemented. 0.2.4 uses the new filename. Prior
`history.db` and `icons.db` are neither imported nor silently deleted.
Remove them yourself only if you intentionally want to discard that data.

For isolated CI and CLI fixtures use `FLUXTOP_DATA_DB` rather than the older
separate history/icon overrides.

## Events and logs

`%LOCALAPPDATA%\FluxTop\activity.log` contains UTC event records with severity,
process ID, thread ID, category and message. Included events: monitor and
launcher lifecycle, sampling cadence and errors, aggregate database opening,
minute commits, rollups, checkpoint alerts and failures, statistical queries,
icon cache operations, overlay settings, autostart and window lifecycle.

Logging uses a dedicated worker and a bounded 256 KiB memory queue. If the queue
is saturated, events are dropped and a backpressure record is emitted. The
logger is diagnostic and is not allowed to block CPU/GPU/network collection.

Rotation: `activity.log` plus `.1` to `.4`, each limited to 100 MiB, so the
five segments have an aggregate budget of 500 MiB. The latest messages are
in the active file. File I/O across launcher and monitor is serialized using a
Windows named mutex. `FLUXTOP_LOG_PATH` overrides the path for tests.

The journal does **not** record every Win32/GPU system call or full per-process
metric vectors: that would damage performance, exhaust 500 MiB rapidly and
potentially disclose local file paths. It records all major application actions
and per-collection summaries. It intentionally omits credentials and API keys.

## Verification

Release + CTest: history, statistics, icons, diagnostics rotation and unified
storage integration. A real Windows overlay run and RAM/disk measurement are
still required; successful unit tests alone do not establish the 50 MiB RAM goal.
