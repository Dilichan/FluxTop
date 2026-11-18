param(
    [string]$BuildDirectory='build',
    [string]$Configuration='Release',
    [string]$Destination='out/release',
    [switch]$SkipTests
)
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory=Join-Path $repository $BuildDirectory }
if (-not [IO.Path]::IsPathRooted($Destination)) { $Destination=Join-Path $repository $Destination }
if (-not $SkipTests) {
    & ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure --timeout 45
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed; release not packaged' }
}
$launcher=Join-Path $BuildDirectory "package/$Configuration/FluxTop.exe"
if (-not (Test-Path -LiteralPath $launcher)) { throw "Missing launcher: $launcher" }
[void](New-Item -ItemType Directory -Path $Destination -Force)
Copy-Item -LiteralPath $launcher -Destination (Join-Path $Destination 'FluxTop.exe') -Force
Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $Destination 'LICENSE.txt') -Force
$files=@('FluxTop.exe','LICENSE.txt')
$lines=foreach ($name in $files) {
    $hash=(Get-FileHash -LiteralPath (Join-Path $Destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
$lines | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding ascii
Get-Item -LiteralPath (Join-Path $Destination 'FluxTop.exe') | Select-Object FullName,Length
