param(
    [string]$RepoPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [ValidateRange(2,20)][int]$Cycles = 3,
    [ValidateRange(2,30)][int]$Samples = 5,
    [ValidateRange(250,10000)][int]$IntervalMs = 500,
    [switch]$NoGpu,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $RepoPath).Path
$exe = Join-Path $root 'build\Release\fluxtop.exe'
if (-not $SkipBuild) {
    & (Join-Path $root 'tools\Check-Regression.ps1') -RepoPath $root
    if ($LASTEXITCODE -ne 0) { throw 'v45 prerequisite CTest failed' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing Release executable: $exe" }
$evidence = Join-Path $root 'build\v45-evidence'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$database = Join-Path $evidence 'integration.db'
$log = Join-Path $evidence 'integration.log'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "v45 mode: elevated=$admin gpu_enabled=$(-not $NoGpu) cycles=$Cycles" -ForegroundColor Cyan
$results = [System.Collections.Generic.List[object]]::new()
for ($cycle=1; $cycle -le $Cycles; $cycle++) {
    $argsList = @('--jsonl','--samples',[string]$Samples,'--interval-ms',[string]$IntervalMs)
    if ($NoGpu) { $argsList += '--no-gpu' }
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $exe
    foreach ($arg in $argsList) { [void]$processInfo.ArgumentList.Add($arg) }
    $processInfo.WorkingDirectory = $root
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.Environment['FLUXTOP_DATA_DB'] = $database
    $processInfo.Environment['FLUXTOP_LOG_PATH'] = $log
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw "Cycle ${cycle}: start failed" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timeout = [Math]::Max(30000, $Samples * $IntervalMs * 6)
        if (-not $process.WaitForExit($timeout)) {
            $process.Kill($true)
            throw "Cycle ${cycle}: process did not exit within $timeout ms"
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $jsonl = Join-Path $evidence ("cycle-{0:D2}.jsonl" -f $cycle)
        $errors = Join-Path $evidence ("cycle-{0:D2}.stderr.txt" -f $cycle)
        [IO.File]::WriteAllText($jsonl, $stdout, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($errors, $stderr, [Text.UTF8Encoding]::new($false))
        if ($process.ExitCode -ne 0) {
            throw "Cycle ${cycle}: exit code 0x$('{0:X8}' -f ([long]$process.ExitCode -band 4294967295L)); stderr=$errors"
        }
        $lines = @($stdout -split '\r?\n' | Where-Object { $_.Trim().Length -gt 0 })
        if ($lines.Count -ne $Samples) { throw "Cycle ${cycle}: expected $Samples snapshots, got $($lines.Count)" }
        $lastTime = [int64]0
        foreach ($line in $lines) {
            $item = $line | ConvertFrom-Json -Depth 64
            if ($item.schema_version -ne 1) { throw "Cycle ${cycle}: JSONL schema changed" }
            foreach ($property in @('timestamp_ms','process_count','system','network_status',
                'disk_io_status','disk_measurement_quality','logical_disks','processes')) {
                if (-not $item.PSObject.Properties[$property]) { throw "Cycle ${cycle}: missing $property" }
            }
            if ($item.timestamp_ms -le $lastTime) { throw "Cycle ${cycle}: non-monotonic timestamp" }
            $lastTime = [int64]$item.timestamp_ms
            if (@('complete','partial','unavailable') -notcontains $item.disk_measurement_quality) {
                throw "Cycle ${cycle}: unsupported disk quality"
            }
            foreach ($p in $item.processes) {
                foreach ($field in @('disk_read_bytes_per_second','disk_write_bytes_per_second')) {
                    $value = $p.$field
                    if ($null -ne $value -and ([double]::IsNaN([double]$value) -or
                        [double]::IsInfinity([double]$value) -or [double]$value -lt 0)) {
                        throw "Cycle ${cycle}: invalid $field for PID $($p.pid)"
                    }
                }
            }
            if ($item.history_error -and $item.history_error.Length -gt 0) {
                throw "Cycle ${cycle}: SQLite history error: $($item.history_error)"
            }
        }
        if (-not (Test-Path -LiteralPath $database)) { throw "Cycle ${cycle}: SQLite database absent" }
        $db = [IO.File]::Open($database,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        try {
            $header = New-Object byte[] 16
            if ($db.Read($header,0,16) -ne 16 -or [Text.Encoding]::ASCII.GetString($header) -ne "SQLite format 3`0") {
                throw "Cycle ${cycle}: invalid SQLite file header"
            }
        } finally { $db.Dispose() }
        $results.Add([pscustomobject]@{
            cycle=$cycle; samples=$lines.Count; elevated=$admin; gpu_enabled=(-not $NoGpu);
            sqlite_bytes=(Get-Item -LiteralPath $database).Length;
            jsonl=$jsonl; stderr=$errors
        })
        Write-Host "Cycle $cycle/$Cycles OK: $($lines.Count) JSONL snapshots; SQLite available" -ForegroundColor Green
    } finally { $process.Dispose() }
}
$summary=Join-Path $evidence 'summary.json'
[IO.File]::WriteAllText($summary,($results | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Host "v45 runtime checks passed. Evidence: $evidence" -ForegroundColor Green
Write-Host 'Run separately without elevation and as administrator; connect/disconnect hardware-monitor providers manually.' -ForegroundColor Yellow
