param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$code = [IO.File]::ReadAllText((Join-Path $Root 'src\overlay.cpp'))
$checks = [ordered]@{
    'Animation is optional' = $code.Contains('bool animation_enabled_ = true;')
    'Persisted in overlay.ini' = ($code.Contains('read(L"overlay_animation", 1)') -and $code.Contains('save(L"overlay_animation", animation_enabled_ ? 1 : 0)'))
    'Checkbox wired to settings' = ($code.Contains('animation_control, 22, 556, 536, 26') -and $code.Contains('case animation_control:'))
    'Focus row stays fixed in Auto' = $code.Contains('if(pinned) from=i==0?0.0f:std::max(1.0f,from);')
    'Header remains static' = $code.Contains('target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,table_width(),')
    '25fps timer on row changes' = ($code.Contains('SetTimer(hwnd_,overlay_animation_timer,40,nullptr)') -and $code.Contains('order_changed'))
    'Stops animations when disabled' = $code.Contains('if (!animation_enabled_) {')
    'Timer stops after transition' = $code.Contains('GetTickCount64()-row_animation_started_>=row_animation_duration_ms_')
    'No numeric crossfade' = (-not $code.Contains('paint_values(*previous,1.0f-progress)'))
    'No icon fade' = $code.Contains('const float opacity=1.0f; // Never fade newly appearing rows or their icons.')
    'No transient burst blink in rows' = (-not $code.Contains('auto_metric_has_burst(p,static_cast<AutoMetric>(j+1))'))
    'No metric pulse color' = $code.Contains('true,false,displayed.loads[column],alpha')
    'Duration uses the sampling interval' = $code.Contains('options_.interval_ms*0.65')
}
$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object {$_.Key})
if ($failed.Count) { throw ('Overlay motion policy failed: ' + ($failed -join '; ')) }
'Overlay motion toggle/no-fade policy passed'