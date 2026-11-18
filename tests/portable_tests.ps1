param([Parameter(Mandatory)][string]$Launcher,[Parameter(Mandatory)][string]$Monitor,[Parameter(Mandatory)][string]$FixtureRoot)
# FluxTop portable SHA256 helper
function Get-FileHash {
    [CmdletBinding()]
    param(
        [string]$LiteralPath,
        [string]$Path,
        [string]$Algorithm = 'SHA256'
    )

    if ($Algorithm -ne 'SHA256') {
        throw "Unsupported hash algorithm: $Algorithm"
    }

    $name = if ($PSBoundParameters.ContainsKey('LiteralPath')) {
        $LiteralPath
    } else {
        $Path
    }

    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($name)
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($resolved)

    try {
        $hash = $sha.ComputeHash($stream)
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }

    [pscustomobject]@{
        Algorithm = 'SHA256'
        Hash = [BitConverter]::ToString($hash).Replace('-', '')
        Path = $resolved
    }
}

$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/UpdateSupport.ps1')
$fixture=Join-Path $FixtureRoot ('portable space-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $fixture -Force)
$arguments=(@('--prepare-only','--data-dir',$fixture) | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
$process=Start-Process -FilePath $Launcher -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Portable extraction failed' }
$hash=(Get-FileHash -LiteralPath $Monitor -Algorithm SHA256).Hash.ToLowerInvariant()
$extracted=Join-Path $fixture "versions/$hash/fluxtop.exe"
if (-not (Test-Path -LiteralPath $extracted)) { throw 'Embedded monitor missing' }
if ((Get-FileHash -LiteralPath $extracted).Hash.ToLowerInvariant() -ne $hash) { throw 'Monitor payload mismatch' }
if (-not (Test-Path -LiteralPath (Join-Path $fixture 'FluxTop.exe'))) { throw 'Stable launcher cache missing' }
$history=Join-Path $fixture 'history.db'
[IO.File]::WriteAllText($history,'history sentinel')
[IO.File]::WriteAllText($extracted,'corrupt cache fixture')
$process=Start-Process -FilePath $Launcher -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -ne 0 -or (Get-FileHash -LiteralPath $extracted).Hash.ToLowerInvariant() -ne $hash) { throw 'Corrupt monitor cache not repaired' }
if ([IO.File]::ReadAllText($history) -ne 'history sentinel') { throw 'History must survive extraction/repair' }
'Single EXE extraction, cache repair and history preservation passed (no UAC or network)'
