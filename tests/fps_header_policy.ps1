param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$overlay=[IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
$layout=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/overlay_layout.hpp'))
$collector=[IO.File]::ReadAllText((Join-Path $Root 'src/fps_collector.cpp'))
$model=[IO.File]::ReadAllText((Join-Path $Root 'src/model.cpp'))
$sampler=[IO.File]::ReadAllText((Join-Path $Root 'src/sampler.cpp'))
$checks=[ordered]@{
 'Five resource columns' = ($layout.Contains('std::array<float, 5> metric_widths') -and $layout.Contains('std::array<const wchar_t*, 5> metric_names'))
 'FPS in application heading' = ($overlay.Contains('std::wstring title = L"') -and $overlay.Contains('foreground_fps_') -and $overlay.Contains('draw_text(title,header[0]'))
 'No FPS table values' = (-not $overlay.Contains('displayed.fps') -and -not $overlay.Contains('std::move(fps),loads,bursts'))
 'No FPS checkbox' = ($overlay.Contains('constexpr unsigned all_metrics = 31;') -and -not $overlay.Contains('metric_visual_to_storage'))
 'Single foreground PID' = ($overlay.Contains('GetWindowThreadProcessId(active_window, &foreground_pid_)') -and $overlay.Contains('fps_targets.push_back(foreground_pid_)'))
 'Every sort mode updates FPS' = ($overlay.Contains('update_view(); // Update FPS target and header in every sorting mode.'))
 'No per-app FPS aggregation' = (-not $model.Contains('max_gpu(combined.presented_fps,part.presented_fps)'))
 'Correct PresentMon codes' = ($collector.Contains('status != 0 && status != 7') -and $collector.Contains('status == 6') -and -not $collector.Contains('status != 0 && status != 8'))
 'Installed loader fallback' = $collector.Contains('PresentMonAPI2Loader.dll')
 'No fake FPS' = ($overlay.Contains('else title += L"') -and $collector.Contains('std::optional<double> best;'))
 'FPS stays on background sampler' = $sampler.Contains('fps_.collect(snapshot.processes, fps_targets_)')
}
$failed=@($checks.GetEnumerator()|Where-Object {-not $_.Value}|ForEach-Object {$_.Key})
if($failed.Count){throw ('FPS foreground policy failed: '+($failed -join '; '))}
'Foreground FPS heading, five metrics, correct PresentMon API status policy passed'
