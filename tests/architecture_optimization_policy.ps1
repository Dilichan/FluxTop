param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$utf8=[Text.UTF8Encoding]::new($false)
$read={param($p) [IO.File]::ReadAllText((Join-Path $Root $p),$utf8)}
$sampler=& $read 'src/sampler.cpp'
$overlay=& $read 'src/overlay.cpp'
$history=& $read 'src/history.cpp'
$process=& $read 'src/process_collector.cpp'
$temperature=& $read 'src/temperatures.cpp'
$checks=[ordered]@{
 'Only one UI aggregation' = ($sampler.Contains('snapshot.grouped_processes = aggregate_application_processes(snapshot.processes)') -and $overlay.Contains('std::move(snapshot_->grouped_processes)') -and -not $overlay.Contains('grouped_processes_=aggregate_application_processes(snapshot_->processes)'))
 'SQLite still consumes raw process history' = ($sampler.Contains('history_.append(snapshot)') -and $history.Contains('for (const auto& row : snapshot.processes)'))
 'Raw UI process allocations are released' = $sampler.Contains('std::vector<Process>().swap(snapshot.processes)')
 'Process count is retained' = $overlay.Contains('std::to_wstring(s.process_count)')
 'GPU collector is optional' = ($sampler.Contains('if (need_gpu && !gpu_)') -and $sampler.Contains('if (!need_gpu) gpu_.reset()'))
 'Network ETW collector is optional' = ($sampler.Contains('if (need_network && !network_)') -and $sampler.Contains('if (!need_network) network_.reset()'))
 'WMI lazily suspends without readers' = ($temperature.Contains('demand_until_.load') -and $temperature.Contains('continue; // Zero WMI queries'))
 'GPU temperature bypasses CPU WMI' = ($sampler.Contains('read_hardware_temperatures() : read_hwinfo_temperatures()'))
 'Win32 is the sole process collector' = ($process.Contains('CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)') -and $process.Contains('GetProcessTimes(') -and $process.Contains('GetProcessMemoryInfo(') -and -not $process.Contains('native_.collect'))
 'Experimental native implementation removed' = (-not (Test-Path (Join-Path $Root 'src/nt_process_snapshot.cpp')) -and -not (Test-Path (Join-Path $Root 'include/fluxtop/nt_process_snapshot.hpp')))
 'FPS sampling remains independent' = ($sampler.Contains('fps_.collect(snapshot.processes, fps_targets_)') -and $overlay.Contains('worker_.set_fps_targets'))
}
$bad=@($checks.GetEnumerator()|Where-Object {-not $_.Value}|ForEach-Object {$_.Key})
if($bad.Count){throw ('Architecture policy failed: '+($bad -join '; '))}
'Architecture demand, fallback, SQLite and memory policy passed'
