$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'FluxTop'
$exe = Join-Path $root 'FluxTop.exe'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\FluxTop'
$shell = New-Object -ComObject WScript.Shell

function Show-Shortcut([string]$label, [string]$path) {
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "${label}: not present"
        return
    }
    $link = $shell.CreateShortcut($path)
    Write-Host "${label}: PRESENT"
    Write-Host "  Target: $($link.TargetPath)"
    Write-Host "  Arguments: $($link.Arguments)"
    Write-Host "  Description: $($link.Description)"
    if ($link.TargetPath -and -not (Test-Path -LiteralPath $link.TargetPath)) {
        Write-Warning "$label shortcut target does not exist"
    }
}

Write-Host '=== FluxTop installation and autostart (read only) ==='
Write-Host "Installed EXE: $exe"
Write-Host "Installed EXE exists: $(Test-Path -LiteralPath $exe)"
$registration = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
if ($registration) {
    Write-Host "Windows installed app: $($registration.DisplayName) $($registration.DisplayVersion)"
    Write-Host "Install location: $($registration.InstallLocation)"
    Write-Host "Uninstall command: $($registration.UninstallString)"
} else { Write-Host 'Windows installed app: not registered' }
$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'FluxTop.lnk'
$startup = Join-Path ([Environment]::GetFolderPath('Startup')) 'FluxTop.lnk'
Show-Shortcut 'Start Menu' $menu
Show-Shortcut 'Startup' $startup
$approvalPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder'
try {
    $value = (Get-ItemProperty -LiteralPath $approvalPath -ErrorAction Stop).'FluxTop.lnk'
    if ($null -ne $value) {
        Write-Host ('StartupApproved first byte: 0x{0:X2}' -f $value[0])
        Write-Host "Windows disabled startup: $(($value[0] -band 1) -ne 0)"
    } else { Write-Host 'StartupApproved: no explicit disable record' }
} catch { Write-Host 'StartupApproved: not available (no override observed)' }
Get-CimInstance Win32_Process -Filter "Name='fluxtop.exe'" -ErrorAction SilentlyContinue |
    Select-Object ProcessId, ParentProcessId, Name, ExecutablePath | Format-Table -AutoSize
Write-Host 'Autostart behavior: Startup folder -> --autostart -> delay 30 s -> UAC -> tray.'
Write-Host 'No settings or registry entries were changed.'
