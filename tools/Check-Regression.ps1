param(
    [string]$RepoPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$SkipConfigure,
    [string]$BuildDirectory = 'build',
    [string]$CmakePath = '',
    [switch]$RejectSkips
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'The complete regression suite requires Administrator; collector skips are not a successful verification.'}
function Invoke-Checked([string]$Exe,[string[]]$Arguments,[string]$Step) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Step failed: exit code $LASTEXITCODE" }
}
$root = (Resolve-Path -LiteralPath $RepoPath).Path
$cmake = if ($CmakePath) { (Resolve-Path -LiteralPath $CmakePath).Path } else {
    Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
}
if (-not $cmake) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "$env:LOCALAPPDATA\Programs\CMake\bin\cmake.exe"
    )
    $cmake = $candidates | ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw 'CMake not found' }
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw 'CTest not found' }
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $root $BuildDirectory }
if (-not $SkipConfigure) { Invoke-Checked $cmake @('-S',$root,'-B',$build) 'configure' }
Invoke-Checked $cmake @('--build',$build,'--config','Release','--parallel') 'Release build'
$report=Join-Path $build 'regression-results.xml'
Invoke-Checked $ctest @('--test-dir',$build,'-C','Release','--output-on-failure','--output-junit',$report) 'full CTest'
[xml]$junit=Get-Content -LiteralPath $report -Raw
foreach($case in $junit.testsuite.testcase){
    $interactiveDisabled = $case.status -eq 'disabled' -and $case.name -in @('windows_UacCancel','windows_StartMenu','windows_Autostart','windows_SleepResume')
    if(!$interactiveDisabled -and ($RejectSkips -or $case.name -in @('admin_required','cpu_etw_live','etw_recovery','collector_stability')) -and ($case.status -eq 'disabled' -or $case.SelectSingleNode('skipped'))){
        throw "Mandatory administrative test was skipped: $($case.name)"
    }
}
# The mandatory behavioural subset must exist, not merely have a matching *policy* name.
$required = @('core','regression_contracts','history','statistics','disk_attribution',
    'temperature_detection','disk_pipeline','autoscore_visibility','chart_controls','jsonl_extended','measurement_labels','integration_pipeline','gpu_memory_fallback','application_display_name','jsonl_contract','smoke','overlay_smoke',
    'architecture_boundaries','temperature_source_selection','intel_dts_decode','intel_dts_cores','cpu_logical','cpu_execution','cpu_view','system_graph_history','control_wheel',
    'build_dependencies','admin_required','cpu_etw_live','etw_recovery','etw_recovery_policy','launcher_stability','collector_stability','history_crash','sensor_recovery','process_enumeration',
    'pawnio_bundle','pawnio_stage_security','pawnio_install_policy','portable','installation_fixture','native_uninstall_fixture','release_update')
$list = & $ctest --test-dir $build -C Release -N
if ($LASTEXITCODE -ne 0) { throw 'CTest test enumeration failed' }
foreach ($name in $required) {
    if (-not ($list | Where-Object { $_ -match (':\s+'+[regex]::Escape($name)+'\s*$') })) {
        throw "Mandatory behavioural test missing: $name"
    }
}
Write-Host 'Release build, full CTest and mandatory behavioural contracts passed.' -ForegroundColor Green
