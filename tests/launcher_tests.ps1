$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/UpdateSupport.ps1')
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $repository ('build/launcher tests ' + [guid]::NewGuid().ToString('N'))
$toolsDirectory = Join-Path $fixture 'tools'
$state = Join-Path $fixture 'out/launcher'
$previousDirectory = Join-Path $state 'versions/previous'
[void](New-Item -ItemType Directory -Path $toolsDirectory -Force)
[void](New-Item -ItemType Directory -Path $previousDirectory -Force)
foreach ($script in @('UpdateSupport.ps1', 'Start-FluxTop.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repository "tools/$script") -Destination $toolsDirectory
}
$previous = @('build/Release/fluxtop.exe', 'build/ninja/fluxtop.exe') | ForEach-Object { Join-Path $repository $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $previous) { throw 'Build the collector before running launcher integration tests' }
$installed = Join-Path $previousDirectory 'fluxtop.exe'
Copy-Item -LiteralPath $previous -Destination $installed
$beforeHash = (Get-FileHash -LiteralPath $installed).Hash
$manifest = Join-Path $state 'last-good.json'
@{ Version = 'previous' } | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
# Deliberately broken candidate: the launcher must retain the previous executable.
Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'message(FATAL_ERROR "Intentional launcher fallback test")'
$powershell = (Get-Command powershell.exe).Source
$launch = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $toolsDirectory 'Start-FluxTop.ps1'), '-NoUpdate')
$expected = (Invoke-FluxProcess -File $installed -Directory $fixture -Arguments @('--version')).Out
$result = Invoke-FluxProcess -File $powershell -Directory $fixture -Arguments ($launch + @('--version')) -TimeoutSeconds 120
if ($result.ExitCode -ne 0 -or $result.Out -ne $expected -or $result.Error -notmatch 'keeping the previous executable') {
    throw "Build failure did not launch fallback: $($result.Error)"
}
if ((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).Version -ne 'previous') { throw 'Last-good manifest changed on failure' }
if ((Get-FileHash -LiteralPath $installed).Hash -ne $beforeHash) { throw 'Previous EXE was overwritten' }
$prepare = Invoke-FluxProcess -File $powershell -Directory $fixture -Arguments ($launch + @('-PrepareOnly')) -TimeoutSeconds 120
if ($prepare.ExitCode -eq 0) { throw 'PrepareOnly hid a failed build' }
$lock = [IO.File]::Open((Join-Path $state 'update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $locked = Invoke-FluxProcess -File $powershell -Directory $fixture -Arguments ($launch + @('--version')) -TimeoutSeconds 15
    if ($locked.ExitCode -eq 0 -or $locked.Out) { throw 'Concurrent launcher bypassed update lock' }
} finally { $lock.Dispose() }

# A clean, already prepared commit must launch without rebuilding, even if its
# CMakeLists would fail: this also checks last-good lookup for SHA-based versions.
Set-Content -LiteralPath (Join-Path $fixture '.gitignore') -Value '/out/'
function Fixture-Git([string[]]$GitArgs) {
    $result = Invoke-FluxGit $fixture (@('-c', 'user.name=FluxTop Tests', '-c', 'user.email=tests@example.invalid') + $GitArgs)
    if ($result.ExitCode -ne 0) { throw "Launcher fixture git failed: $($result.Error)" }
    return $result.Out.Trim()
}
$null = Fixture-Git @('init', '--initial-branch=main')
$null = Fixture-Git @('add', '.gitignore', 'CMakeLists.txt', 'tools')
$null = Fixture-Git @('commit', '-m', 'launcher cache fixture')
$commit = Fixture-Git @('rev-parse', 'HEAD')
$cachedDirectory = Join-Path $state "versions/$commit"
[void](New-Item -ItemType Directory -Path $cachedDirectory)
Copy-Item -LiteralPath $installed -Destination (Join-Path $cachedDirectory 'fluxtop.exe')
@{ Version = $commit } | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
$logHash = (Get-FileHash -LiteralPath (Join-Path $state 'build.log')).Hash
$cached = Invoke-FluxProcess -File $powershell -Directory $fixture -Arguments ($launch + @('--version')) -TimeoutSeconds 30
if ($cached.ExitCode -ne 0 -or $cached.Out -ne $expected -or $cached.Error -match 'preparing Release') { throw 'Prepared commit was not reused' }
if ((Get-FileHash -LiteralPath (Join-Path $state 'build.log')).Hash -ne $logHash) { throw 'Cache hit rebuilt the source' }
'Launcher fallback, locking and cache checks passed'
