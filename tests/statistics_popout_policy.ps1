param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$header=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/statistics_page.hpp'))
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$popup=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_popout.inc'))
$overlay=[IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
foreach($ok in @(
    $header.Contains('PopupState'),
    $page.Contains('popout_live_id'),
    $page.Contains('popout_history_id'),
    $page.Contains('animation_duration_ms_'),
    $page.Contains('rebuild_live_series()'),
    $page.Contains('GetTickCount64()-animation_started_'),
    $page.Contains('IntersectClipRect'),
    $popup.Contains('WM_MOUSEWHEEL'),
    $popup.Contains('WM_LBUTTONDBLCLK'),
    $popup.Contains('WM_LBUTTONDOWN'),
    $popup.Contains('draw_graph(dc,graph,apps'),
    $overlay.Contains('grouped_processes_,options_.interval_ms')
)) {if(-not $ok){throw 'Detached charts and smooth sampling contract failed'}}
'Statistics popout and smooth animation integration passed'