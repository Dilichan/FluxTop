param(
    [string]$BuildDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build/v54'),
    [string]$CtestPath='',
    [ValidateRange(1,50)][int]$Repetitions=5,
    [string]$TestPattern='',
    [ValidateRange(4,60)][int]$SoakSamples=12,
    [switch]$SkipFull
)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this stability suite as Administrator.'}
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
if(-not $CtestPath){
    $command=Get-Command ctest.exe -ErrorAction SilentlyContinue
    if($command){$CtestPath=$command.Source}else{
        $CtestPath=Get-Item "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
$output=Join-Path $BuildDirectory ('stability-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
[void][IO.Directory]::CreateDirectory($output)
$env:FLUXTOP_DATA_DB=Join-Path $output 'monitor.db'
$env:FLUXTOP_LOG_PATH=Join-Path $output 'activity.log'
$env:FLUXTOP_STABILITY_SAMPLES="$SoakSamples"
$results=[Collections.Generic.List[object]]::new()
$metadata=[ordered]@{started_utc=[DateTime]::UtcNow.ToString('o');build=$BuildDirectory;repetitions=$Repetitions;
    windows=[Environment]::OSVersion.VersionString;logical_processors=[Environment]::ProcessorCount;soak_samples=$SoakSamples;
    monitor_sha256=(Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'fluxtop.exe')).Hash}
$metadata | ConvertTo-Json | Set-Content (Join-Path $output 'environment.json') -Encoding UTF8
function Run-Case([string]$Name,[string]$Arguments){
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$CtestPath;$info.Arguments=$Arguments+' --output-junit "'+(Join-Path $output ($Name+'.xml'))+'"';$info.WorkingDirectory=$BuildDirectory
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    $timer=[Diagnostics.Stopwatch]::StartNew();[void]$process.Start()
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit();$code=$process.ExitCode;$timer.Stop()
    $log=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $output ($Name+'.log')),$log)
    if($log -match '(admin_required|cpu_etw_live|etw_recovery|collector_stability)[^\r\n]*\*\*\*Skipped'){$code=77}
    $results.Add([pscustomobject]@{case=$Name;exit_code=$code;seconds=$timer.Elapsed.TotalSeconds;passed=($code -eq 0)})
    $process.Dispose()
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
    Write-Host "$Name : exit=$code"
}
$base='--test-dir "'+$BuildDirectory+'" -C Release --output-on-failure --parallel 1 --no-tests=error'
if($TestPattern){
    for($round=1;$round -le $Repetitions;$round++){Run-Case "selected-$round" ($base+' -R "'+$TestPattern+'"')}
    Write-Host "Evidence: $output"
    if($results.Where({-not $_.passed}).Count){exit 1};exit 0
}
if(-not $SkipFull){Run-Case 'full' $base}
for($round=1;$round -le $Repetitions;$round++){
    Run-Case "install-$round" ($base+' -R "^installation_fixture$"')
    Run-Case "cpu-etw-$round" ($base+' -R "^cpu_etw_live$"')
    Run-Case "launcher-$round" ($base+' -R "^launcher_stability$"')
    Run-Case "etw-recovery-$round" ($base+' -R "^etw_recovery$"')
    Run-Case "history-crash-$round" ($base+' -R "^history_crash$"')
}
Write-Host "Evidence: $output"
if($results.Where({-not $_.passed}).Count){exit 1}
exit 0
