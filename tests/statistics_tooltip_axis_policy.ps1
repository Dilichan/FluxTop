param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$pop=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_popout.inc'))
$header=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/statistics_page.hpp'))
$checks=[ordered]@{
  'Shared renderer'=$page.Contains('void StatisticsPage::draw_metric_tooltip(')
  'Embedded hover'=$page.Contains('draw_metric_tooltip(dc,bounds,source[hover_index_],hover_value_)')
  'Detached hover'=$pop.Contains('draw_metric_tooltip(dc,bubble,app,score)')
  'Current readings from grouped data'=$page.Contains('current_readings_.emplace(color_key(identity)')
  'Consistent network units'=$page.Contains('stat_network(process->rx_bytes_per_second,process->tx_bytes_per_second)')
  'Asymmetric easing'=$page.Contains('double eased_axis(double current,double target,double elapsed_ms)')
  'Smooth target separation'=$header.Contains('target_auto_zoom_')
  'Detached axis easing'=$page.Contains('popup.cached_auto_zoom=eased_axis(')
  'New dataset targets'=$pop.Contains('popup.target_auto_zoom=auto_vertical_zoom(apps,from,to)')
  'Animation continues while easing'=($page.Contains('const bool axis_animating=advance_axes(elapsed);') -and $page.Contains('if((!animating && !axis_animating) ||') -and $page.Contains('(!active_ && !popups_[0].window && !popups_[1].window))') -and $page.Contains('KillTimer(hwnd_,animate_message);'))
}
$failed=@($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object {$_.Key})
if($failed.Count){throw ('Statistics tooltip/axis policy failed: '+($failed -join '; '))}
'Statistics tooltip and auto-axis integration passed'
