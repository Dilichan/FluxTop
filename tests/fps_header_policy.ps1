param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$overlay = [IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
$collector = [IO.File]::ReadAllText((Join-Path $Root 'src/fps_collector.cpp'))
$layout = [IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/overlay_layout.hpp'))
$sampler = [IO.File]::ReadAllText((Join-Path $Root 'src/sampler.cpp'))
$model = [IO.File]::ReadAllText((Join-Path $Root 'src/model.cpp'))
$checks = [ordered]@{
    'Six columns, FPS first' = ($layout.Contains('metric_visual_to_storage{5U, 0U, 1U, 2U, 3U, 4U}') -and $layout.Contains('std::array<float, 6> metric_widths{5.5f, 8.0f, 5.8f, 8.0f, 9.0f, 12.0f}'))
    'Settings follow visual order' = ($overlay.Contains('metric_names[metric], BS_AUTOCHECKBOX') -and $overlay.Contains('const unsigned metric=metric_visual_to_storage[i];') -and $overlay.Contains('metrics_ |= 1U<<metric;'))
    'Settings window fits six boxes' = ($overlay.Contains('const int page_width=client_page_==1?780:680;') -and $overlay.Contains('metric_control+i, 22+static_cast<int>(i)*104, 451, 98, 26'))
    'FPS bit is unchanged' = ($overlay.Contains('constexpr unsigned default_metrics = 31;') -and $overlay.Contains('(metrics_ & (1U<<5))'))
    'FPS per process and group' = ($model.Contains('max_gpu(combined.presented_fps,part.presented_fps)') -and $overlay.Contains('p.presented_fps'))
    'FPS is not in app title' = (-not $overlay.Contains('std::wstring title=L"') -and $overlay.Contains('draw_text(L"') -and $overlay.Contains('FPS",header[i]'))
    'Seven row cells avoid overflow' = ($overlay.Contains('std::array<D2D1_RECT_F,7> cells{};') -and $overlay.Contains('std::array<D2D1_RECT_F,7> rects{};'))
    'Headers use mapped columns' = ($overlay.Contains('const unsigned metric=metric_visual_to_storage[i-1];') -and $overlay.Contains('draw_header_metric(metric,totals[metric],load[metric]'))
    'Rows use mapped columns' = ($overlay.Contains('const unsigned metric=metric_visual_to_storage[position];') -and $overlay.Contains('values[metric],cells[position+1]'))
    'Only displayed apps are tracked' = ($overlay.Contains('worker_.set_fps_targets(std::move(fps_targets));') -and $overlay.Contains('display[i]->member_pids') -and $overlay.Contains('const auto visible_count=std::min(display.size(),static_cast<std::size_t>(options_.top));'))
    'Collector tracks multiple PIDs' = ($collector.Contains('max_tracked_pids = 128') -and $collector.Contains('pmStartTrackingProcess') -and $collector.Contains('row.presented_fps = best'))
    'Collector off when FPS hidden' = ($overlay.Contains('if (visible_ && !summary_only() && (metrics_ & (1U<<5)))'))
    'No fabricated FPS' = ($overlay.Contains(('L"' + [char]0x2014 + '"')) -and $collector.Contains('std::optional<double> best;') -and $collector.Contains('row.presented_fps = best;'))
    'Sampling worker gets PID set' = ($sampler.Contains('sampler.set_fps_targets(std::move(targets));'))
}
$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
if ($failed.Count -ne 0) { throw ('FPS column policy failed: ' + ($failed -join '; ')) }
'FPS-first column, opt-in collection, persistent bits and geometry policy passed'
