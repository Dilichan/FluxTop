param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$header=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/statistics_page.hpp'))
$reader=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_history.cpp'))
foreach($ok in @(
    $page.Contains('popout_table_id'),
    $page.Contains('void StatisticsPage::open_table()'),
    $page.Contains('void StatisticsPage::populate_table()'),
    $page.Contains('fill(table_,image_list_)'),
    $page.Contains('fill(table_window_list_,table_image_list_)'),
    $page.Contains('&& !table_window_) return;'),
    $page.Contains('sorted.size(),100U'),
    $page.Contains('StatisticsPage::hover_dispatch'),
    $page.Contains('WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE'),
    $page.Contains('case WM_NCHITTEST:return HTTRANSPARENT;'),
    $page.Contains('put(limit_label_,18,43,189,27);'),
    $page.Contains('put(history_limit_label_,18,296,147,27);'),
    $page.Contains('put(period_,18,345,165,230);'),
    $page.Contains('put(uptime_limit_label_,18,611,147,27);'),
    $page.Contains('put(uptime_period_,18,650,165,190);'),
    $page.Contains('uptime_period_id && HIWORD(wp)==CBN_SELCHANGE'),
    $header.Contains('uptime_from{},uptime_to{}'),
    $page.Contains('requested_={from,to,uptime_from,uptime_to,'),
    $page.Contains('const auto [uptime_from,uptime_to]=range(uptime_period_index_,uptime_from_,uptime_to_);'),
    $reader.Contains('out.uptime=top_apps(connection.db,uptime_from_ms,uptime_to_ms,'),
    $header.Contains('HWND table_window_'),
    $header.Contains('HWND tooltip_')
)) { if(-not $ok) { throw 'Statistics table/tooltip/top-limit integration failed' } }
'Statistics table popout, consistent limit and native hover passed'
