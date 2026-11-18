[CmdletBinding()]
param(
    [switch]$NoUpdate,
    [switch]$PrepareOnly,
    [switch]$StandardUser,
    [switch]$Autostart,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$AppArguments
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'UpdateSupport.ps1')
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stateDirectory = Join-Path $repository 'out/launcher'
$lock = $null
$exe = $null
if ($Autostart) {
    $env:GIT_TERMINAL_PROMPT = '0'
    $env:GCM_INTERACTIVE = 'Never'
    Start-Sleep -Seconds 30
    $AppArguments = @('--overlay', '--tray')
    # No console survives startup; retain diagnostics on disk.
    [void](New-Item -ItemType Directory -Path $stateDirectory -Force)
    $startupLog = Join-Path $stateDirectory 'autostart.log'
    $writer = [IO.StreamWriter]::new($startupLog, $false, [Text.Encoding]::UTF8)
    $writer.AutoFlush = $true
    [Console]::SetError($writer)
    [Console]::SetOut($writer)
}
try {
    [void](New-Item -ItemType Directory -Path $stateDirectory -Force)
    try {
        $lock = [IO.File]::Open((Join-Path $stateDirectory 'update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    } catch { throw 'Another launcher is updating this checkout; retry after it finishes' }
    $lastGood = Get-FluxLastGood $stateDirectory
    if ($lastGood) { $exe = $lastGood.Exe }
    if (-not $exe) {
        foreach ($candidate in @('build/Release/fluxtop.exe', 'build/ninja/fluxtop.exe')) {
            $path = Join-Path $repository $candidate
            if (Test-Path -LiteralPath $path -PathType Leaf) { $exe = $path; break }
        }
    }
    try {
        if (-not $NoUpdate) {
            try { $update = Update-FluxCheckout $repository }
            catch { $update = 'git-unavailable' }
            [Console]::Error.WriteLine("FluxTop update: $update")
        }
        try { $checkout = Get-FluxCheckout $repository }
        catch { $checkout = [pscustomobject]@{ Commit = ''; Dirty = $true } }
        if ($checkout.Commit -and -not $checkout.Dirty) { $version = $checkout.Commit }
        else { $version = 'local-' + [guid]::NewGuid().ToString('N') }
        if (-not $lastGood -or $lastGood.Version -ne $version) {
            [Console]::Error.WriteLine('FluxTop: preparing Release and running tests...')
            $newExe = Build-FluxVersion $repository $stateDirectory $version
            $temporary = Join-Path $stateDirectory 'last-good.tmp'
            @{ Version = $version } | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
            $manifest = Join-Path $stateDirectory 'last-good.json'
            if (Test-Path -LiteralPath $manifest) { [IO.File]::Replace($temporary, $manifest, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $manifest) }
            $exe = $newExe
        }
    } catch {
        [Console]::Error.WriteLine("FluxTop: $($_.Exception.Message)")
        if (-not $exe) { throw 'No working executable available' }
        [Console]::Error.WriteLine('FluxTop: keeping the previous executable.')
        if ($PrepareOnly) { throw 'Preparation failed; previous executable retained' }
    }
} catch {
    [Console]::Error.WriteLine("FluxTop launcher: $($_.Exception.Message)")
    exit 1
} finally {
    if ($lock) { $lock.Dispose() }
}

if ($PrepareOnly) { exit 0 }
# Request elevation only for the prepared monitor; no resident updater host.
try {
    $start = New-FluxMonitorStartInfo -Executable $exe -Repository $repository -AppArguments $AppArguments -StandardUser:$StandardUser
    if ($Autostart -and -not $StandardUser) { $start.WindowStyle = 'Hidden' }
    Start-Process @start | Out-Null
    exit 0
} catch {
    if (Test-FluxElevationCancelled $_.Exception) {
        [Console]::Error.WriteLine('FluxTop: administrator startup was cancelled in UAC. No monitor was started.')
        exit 1223
    }
    [Console]::Error.WriteLine("FluxTop launcher: $($_.Exception.Message)")
    exit 1
}
