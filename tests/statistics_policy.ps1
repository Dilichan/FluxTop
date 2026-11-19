param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$history=[IO.File]::ReadAllText((Join-Path $Root 'src/history.cpp'))
$math=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/history_math.hpp'))
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$reader=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_history.cpp'))
$overlay=[IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
$checks=[ordered]@{
 'One record per app per completed minute' = ($history.Contains('std::unordered_map<std::string,ApplicationMinute> apps') -and $history.Contains('history_floor(t,history_minute_ms)'))
 'Constant-space numerators/denominators' = ($math.Contains('integral') -and $math.Contains('duration_ms') -and $math.Contains('correction'))
 'No seconds in SQLite' = ($history.Contains('CREATE TABLE history_rollups') -and -not $history.Contains('CREATE TABLE measurements('))
 'UTC bucket conveyor' = ($history.Contains('compact_one(db,0,1,history_hour_ms') -and $history.Contains('compact_one(db,1,2,history_day_ms'))
 'No multiplication of idle app uptime' = $history.Contains('const auto begin=std::max(end-period,last_end_ms);')
 'Long-term daily retention' = $history.Contains('history_retention_days*history_day_ms')
 '50 GiB hard emergency brake' = ($math.Contains('50ULL * 1024 * 1024 * 1024') -and $history.Contains('storage_bytes(path)>=history_stop_bytes'))
 'WAL checkpoint and guard' = ($history.Contains('SQLITE_CHECKPOINT_TRUNCATE') -and $history.Contains('wal_bytes(path)>256*mib'))
 'Background IO thread' = ($history.Contains('thread=std::thread([this]{run();})') -and $history.Contains('ready.wait('))
 'History read-only and cancellable' = ($reader.Contains('SQLITE_OPEN_READONLY') -and $reader.Contains('sqlite3_progress_handler'))
 'Live and historical charts use rollups' = ($reader.Contains('FROM history_rollups') -and $page.Contains('on_snapshot'))
 'System application toggle' = ($reader.Contains('statistics_is_system(') -and $page.Contains('stats_show_system'))
 'Custom date bounds' = $page.Contains('DTM_SETRANGE')
 'FPS and overlay untouched' = $overlay.Contains('statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,10U)')
 'Independent statistics Top' = ($page.Contains('L"stats_top"') -and $page.Contains('stats_limit_id') -and -not $overlay.Contains('statistics_->set_limit(options_.top)'))
}
$failed=@($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
if($failed.Count){throw ('History conveyor policy failed: '+($failed -join '; '))}
'History conveyor v10 mathematics, storage, charts, cancellation and WAL safety passed'
