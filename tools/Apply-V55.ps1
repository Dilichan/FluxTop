[CmdletBinding()]
param(
    [string]$PatchPath,
    [string]$RepoPath = (Join-Path $PSScriptRoot '..'),
    [string]$BuildDirectory = 'build/v55',
    [string]$CMake = 'cmake.exe'
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open an elevated Developer PowerShell for the Release build and full CTest.'
}
$root = (Resolve-Path -LiteralPath $RepoPath).Path
if ($PatchPath) { $PatchPath = (Resolve-Path -LiteralPath $PatchPath).Path }
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

Push-Location -LiteralPath $root
try {
    if ($PatchPath) {
        Invoke-Checked git @('apply','--check','--',$PatchPath)
        Invoke-Checked git @('apply','--',$PatchPath)
    }
    Invoke-Checked $CMake @('-S','.','-B',$BuildDirectory,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release')
    Invoke-Checked $CMake @('--build',$BuildDirectory,'--config','Release','--parallel')
    $runner = Join-Path $root 'tools/Test-Stability.ps1'
    $ctest = Join-Path (Split-Path -Parent (Get-Command $CMake).Source) 'ctest.exe'
    Invoke-Checked powershell.exe @('-NoProfile','-ExecutionPolicy','Bypass','-File',$runner,
        '-BuildDirectory',$BuildDirectory,'-CtestPath',$ctest,'-TestPattern','.','-Repetitions','1')
} finally {
    Pop-Location
}
