param(
    [Parameter(Mandatory)][ValidateSet('UacCancel','StartMenu','Autostart','SleepResume')][string]$Scenario,
    [string]$LauncherPath=(Join-Path $env:LOCALAPPDATA 'FluxTop/FluxTop.exe'),
    [string]$MonitorPath='',
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '../build/lifecycle'),
    [switch]$AllowInteractive
)
$ErrorActionPreference='Stop'
if(-not $AllowInteractive){throw 'Interactive scenario is opt-in. Re-run with -AllowInteractive when ready to operate Windows/UAC.'}
$admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$output=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ($Scenario+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
[void][IO.Directory]::CreateDirectory($output)
function Monitors { @(Get-CimInstance Win32_Process -Filter "Name='fluxtop.exe'" | Where-Object {
    $_.ExecutablePath -and $_.ExecutablePath -ine $LauncherPath -and
    $_.ExecutablePath -ine (Join-Path $env:LOCALAPPDATA 'FluxTop/FluxTop.exe')
}) }
$result=[ordered]@{scenario=$Scenario;started_utc=[DateTime]::UtcNow.ToString('o');passed=$false;details=''}
try {
    if($Scenario -eq 'UacCancel'){
        if($admin){throw 'Run UacCancel from an ordinary, unelevated PowerShell.'}
        if((Monitors).Count){throw 'Close FluxTop before testing cancellation; an existing monitor bypasses the launch path.'}
        $LauncherPath=(Resolve-Path -LiteralPath $LauncherPath).Path
        $fixture=Join-Path $output 'private-install'
        Read-Host 'Press Enter, then choose No in the Windows UAC prompt' | Out-Null
        $child=Start-Process -FilePath $LauncherPath -ArgumentList ('--data-dir "'+$fixture+'"') -PassThru
        if(-not $child.WaitForExit(120000)){throw 'Launcher did not complete after the UAC response.'}
        if($child.ExitCode -ne 1223){throw "Expected ERROR_CANCELLED (1223), got $($child.ExitCode)."}
        Start-Sleep -Seconds 2
        if((Monitors).Count){throw 'A monitor appeared after UAC cancellation.'}
        $result.details='UAC declined; launcher returned 1223; no monitor started.'
    } elseif($Scenario -in @('StartMenu','Autostart')){
        if($admin){throw 'Run shell-launch scenarios from ordinary PowerShell to test the real elevation path.'}
        if((Monitors).Count){throw 'Close FluxTop before testing a fresh shell launch.'}
        $folder=if($Scenario -eq 'StartMenu'){[Environment]::GetFolderPath('Programs')}else{[Environment]::GetFolderPath('Startup')}
        $link=Join-Path $folder 'FluxTop.lnk'
        if(-not (Test-Path -LiteralPath $link)){throw "Required installed shortcut missing: $link"}
        $shortcut=(New-Object -ComObject WScript.Shell).CreateShortcut($link)
        if(-not (Test-Path -LiteralPath $shortcut.TargetPath)){throw 'Shortcut target is missing.'}
        $LauncherPath=$shortcut.TargetPath
        if($Scenario -eq 'Autostart' -and $shortcut.Arguments -notmatch '--autostart'){throw 'Startup shortcut lacks --autostart.'}
        $result.shortcut=@{path=$link;target=$shortcut.TargetPath;arguments=$shortcut.Arguments}
        if($Scenario -eq 'StartMenu'){
            Read-Host 'Open Windows Start, select FluxTop, accept UAC, then press Enter here' | Out-Null
        } else {
            Read-Host 'Press Enter to invoke the actual Startup shortcut; accept UAC after the 30-second delay' | Out-Null
            Start-Process -FilePath $link
        }
        $deadline=[DateTime]::UtcNow.AddSeconds(120)
        do {$running=Monitors;if($running.Count){break};Start-Sleep -Milliseconds 500}while([DateTime]::UtcNow -lt $deadline)
        if($running.Count -ne 1){throw "Expected exactly one monitor, found $($running.Count)."}
        $result.monitor_ids=@($running.ProcessId)
        $result.details='Installed shortcut launched one monitor. Monitor runtime requires administrator; ordinary launch is separately tested by admin_required. Autostart invocation does not prove a real Windows logon.'
        # Leave this user-launched monitor open; never kill an existing client.
    } else {
        if(-not $admin){throw 'Run SleepResume as Administrator to start its own collector.'}
        $MonitorPath=(Resolve-Path -LiteralPath $MonitorPath).Path
        $env:FLUXTOP_DATA_DB=Join-Path $output 'history.db'
        $env:FLUXTOP_LOG_PATH=Join-Path $output 'activity.log'
        $stdout=Join-Path $output 'samples.jsonl';$stderr=Join-Path $output 'stderr.log'
        $child=Start-Process -FilePath $MonitorPath -ArgumentList '--jsonl --interval-ms 1000 --top 100' -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
        try {
            Start-Sleep -Seconds 8
            $before=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            Read-Host 'Put Windows to sleep for at least 15 seconds, wake it, then press Enter. This script does not change power state' | Out-Null
            $wake=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            if($wake-$before -lt 15000){throw 'Sleep scenario was too short; no valid sleep/resume observation.'}
            Start-Sleep -Seconds 20
            if($child.HasExited){throw "Collector exited during sleep/resume: $($child.ExitCode)"}
            $rows=@(Get-Content -LiteralPath $stdout | Where-Object {$_ -match '^\{.*\}$'} | ForEach-Object {$_ | ConvertFrom-Json})
            $post=@($rows | Where-Object {$_.timestamp_ms -ge $wake})
            if($post.Count -lt 5){throw 'Too few fresh post-resume samples.'}
            if(-not ($post | Where-Object {$_.cpu_execution.status -in @('ready','partial')})){throw 'CPU ETW did not recover after resume.'}
            if(-not ($post | Where-Object {$_.network_status -eq 'ready'})){throw 'Network did not recover after resume.'}
            if($post | Where-Object {$null -ne $_.cpu_temperature_c -and $_.cpu_temperature_c -le 0}){throw 'Invalid zero CPU temperature after resume.'}
            $gap=0;for($i=1;$i -lt $rows.Count;$i++){$gap=[Math]::Max($gap,$rows[$i].timestamp_ms-$rows[$i-1].timestamp_ms)}
            if($gap -lt 10000){throw 'No suspension gap in collector timestamps; scenario not verified.'}
            $result.details="Fresh samples and collectors recovered after observed suspension gap ${gap}ms. Sensor hardware freshness is also covered by deterministic expiry tests."
        } finally {if(-not $child.HasExited){$child.Kill();$child.WaitForExit()};$child.Dispose()}
    }
    $result.passed=$true
} catch {$result.details=$_.Exception.Message;throw} finally {
    $result.finished_utc=[DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'result.json') -Encoding UTF8
    Write-Host "Evidence: $output"
}
