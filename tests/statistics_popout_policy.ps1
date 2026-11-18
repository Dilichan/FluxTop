param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$header = [IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/statistics_page.hpp'))
$page = [IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$popup = [IO.File]::ReadAllText((Join-Path $Root 'src/statistics_popout.inc'))
$overlay = [IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
$checks = [ordered]@{
    'Two detachable graphs' = ($header.Contains('PopupState') -and $page.Contains('popout_live_id') -and $page.Contains('popout_history_id'))
    'Synchronized live drawing' = ($page.Contains('rebuild_live_series()') -and $page.Contains('animation_duration_ms_') -and $page.Contains('GetTickCount64()-animation_started_'))
    'Clipped graph rendering' = $page.Contains('IntersectClipRect')
    'Independent statistics limit' = ($page.Contains('stats_top') -and $page.Contains('stats_limit_id'))
    'Smooth animation timer' = ($page.Contains('animate_message') -and $page.Contains('SetTimer(hwnd_,animate_message,33,nullptr)'))
    'Mouse zoom and pan' = ($popup.Contains('WM_MOUSEWHEEL') -and $popup.Contains('WM_LBUTTONDOWN') -and $popup.Contains('WM_LBUTTONDBLCLK'))
    'Graph rendering in popout' = ($popup.Contains('draw_graph(') -and $popup.Contains('paint_popup('))
    'Detachable popout not owned by settings window' = ($popup.Contains('1050,590,nullptr,nullptr,GetModuleHandleW(nullptr),&popup)'))
    'Detachable table not owned by settings window' = ($page.Contains('nullptr,nullptr,GetModuleHandleW(nullptr),this);'))
    'Smooth axis target cached between samples' = ($page.Contains('target_auto_zoom_[index]') -and $page.Contains('axis_initialized_[index]') -and $page.Contains('popup.axis_dirty=true') -and $header.Contains('target_auto_zoom_'))
    'Stable time scale during mouse hover' = ($page.Contains('const auto now=last_live_timestamp_>0?last_live_timestamp_:now_ms();') -and $popup.Contains('last_live_timestamp_>0?last_live_timestamp_:now_ms()'))
    'Sampler interval forwarded to graphs' = $overlay.Contains('grouped_processes_,options_.interval_ms')
    'Minimizing client does not close it' = ($overlay.Contains('if (wp==SIZE_MINIMIZED) return 0;') -and -not $overlay.Contains('PostMessageW(window,WM_CLOSE,0,0)'))
}
$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
if ($failed.Count -gt 0) {
    throw ('Statistics popout contract failed: ' + ($failed -join '; '))
}
'Popout, animation, stable time-axis and independent window policy passed'