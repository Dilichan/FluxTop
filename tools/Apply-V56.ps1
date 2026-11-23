[CmdletBinding()]
param(
    [string]$RepoPath = (Join-Path $PSScriptRoot '..'),
    [string]$PatchPath,
    [string]$BuildDirectory = ('build/v56-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')),
    [string]$CMake = 'cmake.exe'
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Start an elevated x64 Developer PowerShell.'
}
$root = (Resolve-Path -LiteralPath $RepoPath).Path
$cmakePath = (Get-Command $CMake -CommandType Application).Source
if ($PatchPath) { $PatchPath = (Resolve-Path -LiteralPath $PatchPath).Path }
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $root
$savedDb = $env:FLUXTOP_DATA_DB
$savedLog = $env:FLUXTOP_LOG_PATH
try {
    $build = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
    if (!$build.StartsWith((Join-Path $root 'build') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Use a fresh directory under build/.'
    }
    if (Test-Path -LiteralPath $build) { throw "The build directory already exists: $build" }
    if ($PatchPath) {
        Invoke-Checked git @('apply','--check','--',$PatchPath)
        Invoke-Checked git @('apply','--',$PatchPath)
    }
    Invoke-Checked $cmakePath @('-P','tools/Prepare-PawnIO.cmake')
    $pawnio = Join-Path $root 'assets/pawnio/PawnIO_setup.exe'
    if ((Get-AuthenticodeSignature -LiteralPath $pawnio).Status -ne 'Valid') {
        throw 'The pinned PawnIO installer did not pass Authenticode verification on this computer.'
    }
    $version = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),'project\(FluxTop VERSION (\d+\.\d+\.\d+)').Groups[1].Value
    if (!$version) { throw 'Cannot determine FluxTop project version.' }
    $tag = "v$version-preview.56"
    Invoke-Checked $cmakePath @('-S',$root,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DFLUXTOP_RELEASE_TAG:STRING=$tag")
    $env:FLUXTOP_DATA_DB = Join-Path $build 'acceptance-history.db'
    $env:FLUXTOP_LOG_PATH = Join-Path $build 'acceptance-activity.log'
    Invoke-Checked powershell.exe @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'tools/Check-Regression.ps1'),
        '-RepoPath',$root,'-SkipConfigure','-BuildDirectory',$build,'-CmakePath',$cmakePath,'-RejectSkips')

    $package = Join-Path $build 'distribution'
    Invoke-Checked $cmakePath @("-DROOT=$root","-DBUILD_DIRECTORY=$build","-DOUTPUT_DIR=$package",'-DCONFIG=Release','-P','tools/Package-Release.cmake')
    $launcher = Join-Path $package 'FluxTop.exe'
    $monitor = Join-Path $build 'fluxtop.exe'
    $manifest = @{}
    foreach ($line in (Get-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt'))) {
        if ($line -notmatch '^([0-9a-f]{64})  (FluxTop\.exe|LICENSE\.txt)$' -or $manifest.ContainsKey($Matches[2])) {
            throw 'Invalid package checksum manifest.'
        }
        $manifest[$Matches[2]] = $Matches[1]
    }
    if ($manifest.Count -ne 2 -or @(Get-ChildItem -LiteralPath $package -Force).Count -ne 3) { throw 'Unexpected package contents.' }
    foreach ($file in $manifest.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $package $file)).Hash -ne $manifest[$file]) { throw "Checksum mismatch: $file" }
    }
    foreach ($exe in $launcher,$monitor) {
        $stream = [IO.File]::OpenRead($exe)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            $stream.Position=0x3c
            $stream.Position=$reader.ReadUInt32()
            if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw "Invalid x64 PE: $exe" }
        } finally { $reader.Dispose() }
    }
    Invoke-Checked (Join-Path $build 'fluxtop_release_tests.exe') @($launcher,$build,$tag)
    Invoke-Checked (Join-Path $build 'fluxtop_pawnio_bundle_tests.exe') @($launcher)
    $fixture = Join-Path $build 'fluxtop_native_launcher_fixture_tests.exe'
    Invoke-Checked $fixture @('portable',$launcher,$monitor,$build)
    Invoke-Checked $fixture @('installation_fixture',$launcher,$monitor,$build)
    [ordered]@{
        base_commit = (& git rev-parse HEAD)
        source_changes = @(& git status --porcelain)
        project_version = $version
        embedded_tag = $tag
        launcher_sha256 = $manifest['FluxTop.exe']
        launcher_signature = [string](Get-AuthenticodeSignature -LiteralPath $launcher).Status
        pawnio_signature = 'Valid'
        local_automatic_acceptance = $true
        public_release_acceptance = $false
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $build 'candidate.json') -Encoding UTF8
    Write-Host "Verified local candidate: $launcher"
} finally {
    $env:FLUXTOP_DATA_DB = $savedDb
    $env:FLUXTOP_LOG_PATH = $savedLog
    Pop-Location
}
