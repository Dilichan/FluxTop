param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference='Stop'
$page=[IO.File]::ReadAllText((Join-Path $Root 'src/statistics_page.cpp'))
$header=[IO.File]::ReadAllText((Join-Path $Root 'include/fluxtop/statistics_page.hpp'))
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
    $page.Contains('put(limit_label_,18,17,315,27);'),
    $header.Contains('HWND table_window_'),
    $header.Contains('HWND tooltip_')
)) { if(-not $ok) { throw 'Statistics table/tooltip/top-limit integration failed' } }
'Statistics table popout, consistent limit and native hover passed'
