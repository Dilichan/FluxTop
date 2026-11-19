# FluxTop History Conveyor v10 — 10-year bounded history

The application samples CPU/RAM/GPU/network exactly as before, using the configured
1–10 second interval. **No new CPU, RAM or GPU measurement algorithm is used.**

## Mathematics

Each per-application observation contains valid metric values x_i and a measured
interval dt_i, limited to the actual sample period (max 10 seconds). We split
intervals crossing UTC minute boundaries. For each metric we accumulate:

- numerator S = sum(x_i * dt_i)
- denominator T = sum(dt_i) for *valid* metric samples only
- minimum, maximum and count
- observed uptime U = sum(dt_i) once for the aggregated application, not per PID

Mean = S/T, only when T > 0. Missing GPU/network samples have T=0, not zero
readings. The sum uses Neumaier-compensated addition in memory. At each time
resolution, merge `(S,T,U)` by addition, `min` by minimum, `max` by maximum.
Never take the unweighted mean of minute/hour averages.

## Pipeline

- Live sampling: in memory; for each unique app, just a fixed 7-metric accumulator.
- Completed UTC minute: one bounded asynchronous SQLite transaction, one row per
  application. No per-process, per-second rows are stored.
- Completed UTC hour: merge all available minute buckets into the corresponding
  hour; delete sources atomically. A game that did not run for an hour has no
  invented rows, and its other days still merge normally.
- Completed UTC day: the same operation for hour buckets.
- Daily buckets: retained for 3,660 days (at least ten calendar years).

The pipeline advances based on **time boundaries**, not on reaching 60 recordings
for an individual application. App inactivity and restarts require no special case.
At most four completed hours/days are consolidated per minute of maintenance, so
catch-up work is bounded.

## Disk and concurrency protections

- Only the dedicated worker touches SQLite for writing/compaction/checkpoint.
  The main collection thread performs only O(apps * metrics) accumulator updates.
- The queue is bounded to 8 completed minutes. If disk is unavailable, errors
  are visible and the sampler continues without blocking.
- The progress handler bounds each historical query to 3 seconds. GUI requests
  are cancellable, preventing infinitely held WAL reader snapshots.
- Passive checkpoint after each minute; nonblocking attempt to truncate WAL when
  it is larger than 64 MiB. Stop history writing if WAL remains above 256 MiB.
- Stop history writing at 49 GiB total to retain at least 1 GiB headroom below
  the **50 GiB** emergency maximum (`db + wal + shm`). This is a safety brake,
  not a target size.
- Incremental vacuum instead of a whole-file VACUUM.
- SQLite `user_version=2` for **fresh** databases; previous v1 histories are not
  automatically modified or deleted. If migrating, preserve/convert separately.

## Reliability / accuracy boundaries

- The last open minute is kept in RAM and flushed on normal shutdown; a power
  failure may lose up to that minute, as explicitly traded for minimal I/O.
- Day-scale history cannot recover sub-day peaks or exact interval boundaries:
  the preserved statistics are exact for observed samples, but time resolution
  intentionally decreases. A partial-day query is necessarily approximate.
- Older daily data and the in-memory 60-second chart are separate streams.
- SQLite WAL checkpoint may be blocked by another reader. In that case the
  capture pipeline stops history recording *before* WAL can grow unchecked.

## Testing

Run CMake Release and CTest on Windows. The history and statistics fixtures test
weighted merges, inactive-app gaps, restarts, retention-tier transitions, uptime
and graph availability. The source-level statistics policy guards the new schema.
No DirectX/FPS source or overlay renderer is changed by this patch.
