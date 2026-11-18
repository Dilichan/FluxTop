param(
    [Parameter(Mandatory=$true)][string]$Launcher,
    [Parameter(Mandatory=$true)][string]$FixtureRoot
)
function Get-FileHash {
    param([Parameter(Mandatory=$true)][string]$LiteralPath)

    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($LiteralPath)

    try {
        $digest = $sha.ComputeHash($stream)
        [pscustomobject]@{
            Hash = [BitConverter]::ToString($digest).Replace('-', '')
        }
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/UpdateSupport.ps1')
$folder=Join-Path $FixtureRoot ('install-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder -Force | Out-Null
try {
    $history=Join-Path $folder 'history.db'
    [IO.File]::WriteAllText($history,'history remains unchanged')
    # --data-dir enables a fixture-only install: never creates real shortcuts or registry keys.
    $arguments = (@('--install-only','--data-dir',$folder) |
        ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $process=Start-Process -FilePath $Launcher -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Fixture installation failed' }
    $installed=Join-Path $folder 'FluxTop.exe'
    if (-not (Test-Path -LiteralPath $installed)) { throw 'Installed entrypoint missing' }
    if ((Get-FileHash -LiteralPath $Launcher).Hash -ne (Get-FileHash -LiteralPath $installed).Hash) {
        throw 'Installed copy differs from download'
    }
    $process=Start-Process -FilePath $Launcher -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Repeat installation failed' }
    if ([IO.File]::ReadAllText($history) -ne 'history remains unchanged') {
        throw 'Installer modified user history'
    }
    Write-Host 'Install fixture: stable launcher copy, idempotence, history preservation passed.'
}
finally {
    Remove-Item -LiteralPath $folder -Force -Recurse -ErrorAction SilentlyContinue
}
