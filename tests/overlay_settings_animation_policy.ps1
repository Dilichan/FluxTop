param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$overlay = [IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'))

function Require-Contains([string]$name, [string]$fragment) {
    if (-not $script:overlay.Contains($fragment)) {
        throw "Overlay settings/animation policy failed: $name"
    }
}
function Require-NotContains([string]$name, [string]$fragment) {
    if ($script:overlay.Contains($fragment)) {
        throw "Overlay settings/animation policy failed: $name"
    }
}

# Both editable settings must use Win32 trackbars, not a combobox/edit field.
Require-Contains 'font trackbar' 'font_control, 290, 146, 268, 32'
Require-Contains 'font range 5..42' 'MAKELPARAM(5,42)'
Require-Contains 'font value read' 'font_control,TBM_GETPOS'
Require-Contains 'interval trackbar' 'interval_control, 290, 378, 268, 32'
Require-Contains 'interval range 1..10' 'MAKELPARAM(1,10)'
Require-Contains 'interval value read' 'interval_control,TBM_GETPOS'
Require-Contains 'font setting persisted' 'save(L"font_size"'
Require-Contains 'interval setting persisted' 'save(L"interval_ms"'
Require-Contains 'immediate label update' 'const bool is_font='
Require-Contains 'debounced slider changes' 'SetTimer(window,settings_timer,150,nullptr)'
Require-Contains 'slider changes applied' 'apply_client();'
Require-Contains 'Statistics starts at overlay Top' 'statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top)'
Require-Contains 'Statistics tab reconciles delayed Top' 'if(page==1 && client_page_!=1) {'
Require-Contains 'Statistics tab reads pending Top' 'const auto pending_top=SendDlgItemMessageW(client_,top_control,TBM_GETPOS,0,0);'
Require-Contains 'Statistics tab commits pending Top' 'static_cast<unsigned>(pending_top)!=options_.top) {'
Require-Contains 'Statistics tab mirrors authoritative Top' 'if(statistics_) statistics_->set_limit(options_.top);'
Require-NotContains 'no old font combobox' 'CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())'
Require-NotContains 'no old interval edit' 'GetDlgItemInt(client_, interval_control'

# Keep the existing rendering optimizations and do not restore fading.
Require-Contains 'PID indexed lookup' 'std::unordered_multimap<std::uint32_t,std::size_t> previous_by_pid'
Require-Contains 'PID lookup by range' 'previous_by_pid.equal_range(rows_[i].pid)'
Require-Contains 'visible range start' 'first_visible=std::min(scroll_,row_motion_.size())'
Require-Contains 'visible range end' 'last_visible=std::min(row_motion_.size(),first_visible+visible_count)'
Require-Contains 'render frame backpressure' 'row_animation_next_frame_tick_'
Require-Contains 'slow frame diagnostics' 'overlay.animation_slow'
Require-Contains 'adaptive render budget' 'frame_cost+8ULL,40ULL,160ULL'
Require-Contains 'dirty region' 'bool partial_body=false;'
Require-Contains 'skip untouched rows' 'if(partial_body && (y+row_height()+2<dirty_top || y-2>dirty_bottom))continue;'
Require-Contains 'no animation fade' 'const float opacity=1.0f; // Never fade newly appearing rows or their icons.'

# Auto-first dropdown regression checks (PowerShell 5.1 compatible).
# Use .NET regex Unicode escapes so this test file stays ASCII-only.
if (-not [regex]::IsMatch($script:overlay, 'for \(const auto label : \{L"\u0410\u0432\u0442\u043e", L"CPU", L"RAM", L"GPU"')) {
    throw 'Sorting policy failed: Auto must be the first dropdown entry.'
}
if ([regex]::IsMatch($script:overlay, 'L"\u0410\u0432\u0442\u043e\s*[\u2014-]')) {
    throw 'Sorting policy failed: the old Auto caption is still present.'
}
Require-Contains 'explicit sort mapping' 'sort_ui_values{Sort::automatic, Sort::cpu, Sort::ram, Sort::gpu'
Require-Contains 'initial selection mapped' 'sort_ui_index(options_.sort)'
Require-Contains 'selected item mapped' 'sort_ui_values[static_cast<std::size_t>(selected_sort)]'
Require-Contains 'GPU restriction based on mode' 'new_sort != Sort::gpu && new_sort != Sort::vram'

'Overlay settings/animation policy passed'
