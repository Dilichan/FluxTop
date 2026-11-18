param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$history=[IO.File]::ReadAllText((Join-Path $Root 'src/history.cpp'))
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$reader=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_history.cpp'))
$overlay=[IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))
foreach($condition in @(
    $history.Contains('CREATE TABLE IF NOT EXISTS application_scores'),
    $history.Contains('auto_rank(group,31U)'),
    $history.Contains('INSERT OR REPLACE INTO application_scores'),
    $reader.Contains('SQLITE_OPEN_READONLY'),
    $reader.Contains('sqlite3_progress_handler'),
    $reader.Contains('GROUP BY p.application_id,s.id'),
    $page.Contains('stats_show_system'),
    $page.Contains('DTM_SETRANGE'),
    $page.Contains('on_snapshot'),
    $overlay.Contains('TCN_SELCHANGE'),
    $page.Contains('L"stats_top"'),
    $page.Contains('stats_limit_id'),
    $page.Contains('StatisticsPage::prepare_colors('),
    $overlay.Contains('statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,10U)'),
    -not $overlay.Contains('statistics_->set_limit(options_.top)')
)) { if(-not $condition) {throw 'Statistics integration contract failed'} }
'FluxTop statistics integration contract passed'