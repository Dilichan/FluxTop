[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/Release/fluxtop.exe'),
    [ValidateRange(1, 86400)][int]$Samples = 600,
    [ValidateRange(0, 3600)][int]$WarmupSamples = 30,
    [ValidateRange(250, 60000)][int]$IntervalMs = 1000,
    [switch]$NoGpu
)

$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$collectorArgs = @('--samples', ($Samples + $WarmupSamples), '--interval-ms', $IntervalMs, '--top', '1', '--jsonl')
if ($NoGpu) { $collectorArgs += '--no-gpu' }
$measurements = [System.Collections.Generic.List[object]]::new()
$state = @{ Seen = 0; PreviousElapsed = 0.0 }

# Keep only self metrics. Process names and paths are never included in this report.
& $resolvedExecutable @collectorArgs | ForEach-Object {
    $sample = $_ | ConvertFrom-Json
    $delta = $sample.elapsed_ms - $state.PreviousElapsed
    $state.PreviousElapsed = $sample.elapsed_ms
    $state.Seen++
    if ($state.Seen -gt $WarmupSamples) {
        if ($null -eq $sample.self.working_set_bytes -or $null -eq $sample.self.private_bytes) {
            throw 'Self memory metrics unavailable; refusing to report unknown values as zero'
        }
        $measurements.Add([pscustomobject]@{
            CPU = $sample.self.cpu_percent
            DurationMs = $delta
            WorkingSetMiB = $sample.self.working_set_bytes / 1MB
            PrivateMiB = $sample.self.private_bytes / 1MB
            CollectionMs = $sample.collection_ms
            Handles = $sample.self.handles
            Processes = $sample.process_count
            Inaccessible = $sample.inaccessible
            GPU = $sample.gpu_status
        })
    }
}
if ($LASTEXITCODE -ne 0) { throw "Collector exited with code $LASTEXITCODE" }
if ($measurements.Count -ne $Samples) { throw "Expected $Samples measured samples, got $($measurements.Count)" }

function Get-Distribution([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    $middle = [int][math]::Floor($ordered.Count / 2)
    $median = $ordered[$middle]
    if ($ordered.Count % 2 -eq 0) { $median = ($ordered[$middle - 1] + $ordered[$middle]) / 2 }
    [pscustomobject]@{
        Median = [math]::Round($median, 3)
        P95 = [math]::Round($ordered[[math]::Ceiling($ordered.Count * 0.95) - 1], 3)
        Max = [math]::Round($ordered[-1], 3)
    }
}
$cpuSamples = @($measurements | Where-Object { $null -ne $_.CPU -and $_.DurationMs -gt 0 })
$weightedCPU = 0.0
$cpuDuration = 0.0
foreach ($sample in $cpuSamples) {
    $weightedCPU += $sample.CPU * $sample.DurationMs
    $cpuDuration += $sample.DurationMs
}

[pscustomobject]@{
    Samples = $measurements.Count
    WarmupSamples = $WarmupSamples
    IntervalMs = $IntervalMs
    GPURequested = -not $NoGpu.IsPresent
    GPUStatuses = @($measurements.GPU | Sort-Object -Unique)
    CPUAveragePercent = $(if ($cpuDuration -gt 0) { [math]::Round($weightedCPU / $cpuDuration, 4) } else { $null })
    CPUValidSamples = $cpuSamples.Count
    WorkingSetMiB = Get-Distribution $measurements.WorkingSetMiB
    PrivateMiB = Get-Distribution $measurements.PrivateMiB
    CollectionMs = Get-Distribution $measurements.CollectionMs
    HandlesStart = $measurements[0].Handles
    HandlesEnd = $measurements[-1].Handles
    ProcessesMin = ($measurements.Processes | Measure-Object -Minimum).Minimum
    ProcessesMax = ($measurements.Processes | Measure-Object -Maximum).Maximum
    InaccessibleMax = ($measurements.Inaccessible | Measure-Object -Maximum).Maximum
    Note = 'CLI-only self metrics; excludes system PDH costs and future overlay/history. JSONL export enabled.'
} | ConvertTo-Json -Depth 4
