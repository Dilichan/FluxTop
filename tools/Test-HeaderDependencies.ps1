param([string]$CmakePath,[string]$CompilerPath,[string]$NinjaPath,[string]$BuildRoot,[string]$SourceRoot,[switch]$Unfixed)
$ErrorActionPreference='Stop'
$compiler=$CompilerPath.Replace('/','\')
$split=$compiler.IndexOf('\VC\',[StringComparison]::OrdinalIgnoreCase)
if($split -lt 0){throw 'Cannot locate Visual Studio for the native dependency fixture.'}
$vsRoot=$compiler.Substring(0,$split)
Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$fix=if($Unfixed){'OFF'}else{'ON'}
& $CmakePath "-DBINARY_ROOT=$BuildRoot" "-DSOURCE_ROOT=$SourceRoot" "-DCOMPILER=$CompilerPath" "-DNINJA=$NinjaPath" "-DFIX_ENVIRONMENT=$fix" -P (Join-Path $PSScriptRoot 'Test-HeaderDependencies.cmake')
exit $LASTEXITCODE
