$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/UpdateSupport.ps1')
$admin = New-FluxMonitorStartInfo -Executable 'C:\Program Files\FluxTop\fluxtop.exe' -Repository 'C:\Program Files\FluxTop' -AppArguments @('--samples', '1')
if ($admin.Verb -ne 'RunAs' -or $admin.ContainsKey('NoNewWindow')) { throw 'Default launch must request UAC without incompatible NoNewWindow' }
if ($admin.ArgumentList -ne '"--samples" "1"') { throw 'App arguments were not preserved' }
$standard = New-FluxMonitorStartInfo -Executable 'fluxtop.exe' -Repository $PSScriptRoot -StandardUser
if ($standard.ContainsKey('Verb') -or -not $standard.NoNewWindow) { throw 'Inherited mode must not request elevation' }
if ($standard.ContainsKey('ArgumentList')) { throw 'Empty argv should not add empty argument' }
$cancelled = [ComponentModel.Win32Exception]::new(1223)
if (-not (Test-FluxElevationCancelled $cancelled)) { throw 'UAC cancellation not recognized' }
$wrapped = [Exception]::new('wrapper', $cancelled)
if (-not (Test-FluxElevationCancelled $wrapped)) { throw 'Wrapped UAC cancellation not recognized' }
if (Test-FluxElevationCancelled ([ComponentModel.Win32Exception]::new(5))) { throw 'Access denied is not UAC cancellation' }
if (Test-FluxElevationCancelled ([Exception]::new('failure'))) { throw 'Unrelated exception classified as cancellation' }
'Elevation selection and UAC cancellation checks passed (no UAC dialog opened)'
