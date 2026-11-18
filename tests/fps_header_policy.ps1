param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$overlay=[IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
$collector=[IO.File]::ReadAllText((Join-Path $Root 'src/fps_collector.cpp'))
$header=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/overlay_layout.hpp'))
$sampler=[IO.File]::ReadAllText((Join-Path $Root 'src/sampler.cpp'))
$model=[IO.File]::ReadAllText((Join-Path $Root 'src/model.cpp'))
$checks=[ordered]@{
    'Separate sixth FPS metric' = ($header.Contains('L"Rx/Tx", L"FPS"') -and $header.Contains('std::array<float, 6> metric_widths'))
    'FPS checkbox in settings' = ($overlay.Contains('metric_names.size()') -and $overlay.Contains('metric_control+i') -and $overlay.Contains('const int page_width=client_page_==1?780:680;'))
    'FPS per process and group' = ($model.Contains('max_gpu(combined.presented_fps,part.presented_fps)') -and $overlay.Contains('p.presented_fps'))
    'Not in the application header' = (-not $overlay.Contains('std::wstring title=L"Приложение ";'))
    'FPS in sixth cell' = ($overlay.Contains('displayed.network,displayed.fps') -and $overlay.Contains('column==4 || column==5'))
    'Only visible PIDs are selected' = ($overlay.Contains('worker_.set_fps_targets(std::move(fps_targets));') -and $overlay.Contains('p.member_pids'))
    'Collector tracks several PIDs' = ($collector.Contains('max_tracked_pids = 128') -and $collector.Contains('pmStartTrackingProcess') -and $collector.Contains('row.presented_fps = best'))
    'Collector disabled when FPS not selected' = ($overlay.Contains('if (visible_ && !summary_only() && (metrics_ & (1U<<5)))'))
    'No fake FPS' = ($overlay.Contains('L"—"') -and $collector.Contains('std::optional<double> best;'))
    'Sampling worker gets target set' = ($sampler.Contains('sampler.set_fps_targets(std::move(targets));'))
}
$failed=@($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object {$_.Key})
if($failed.Count){throw ('FPS column policy failed: '+($failed -join '; '))}
'Per-application FPS column, default-off collection and settings policy passed'
