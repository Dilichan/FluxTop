param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$launcher = [IO.File]::ReadAllText((Join-Path $Root 'src/launcher.cpp'),[Text.Encoding]::UTF8)
$overlay = [IO.File]::ReadAllText((Join-Path $Root 'src/overlay.cpp'),[Text.Encoding]::UTF8)
if ($launcher.Contains('if (!prepare && fluxtop::try_release_update(')) { throw 'Silent startup update was reintroduced' }
if (-not $launcher.Contains('if (check_update) {')) { throw 'Explicit update mode is missing' }
if (-not $launcher.Contains('MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2')) { throw 'User consent is missing' }
if (-not $launcher.Contains('if (MessageBoxW(nullptr,question.c_str()')) { throw 'Update prompt must precede download' }
if (-not $overlay.Contains('check.lpParameters=L"--check-update";')) { throw 'Client does not trigger update check' }
if (-not $launcher.Contains('close_monitor_for_consented_update()')) { throw 'Monitor shutdown is missing' }
$resource = [IO.File]::ReadAllText((Join-Path $Root 'src/launcher.rc.in'),[Text.Encoding]::UTF8)
if (-not $resource.Contains('204 L"FluxTop.update-consent.v1"')) { throw 'Launcher capability marker is missing' }
if (-not $overlay.Contains('std::wstring_view(marker)==L"FluxTop.update-consent.v1"')) { throw 'Client does not detect legacy launcher capability' }
if ($launcher.IndexOf('fluxtop::download_release_update(*candidate)') -lt $launcher.IndexOf('MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2')) { throw 'Update downloads before consent' }
if ($launcher.IndexOf('if (!close_monitor_for_consented_update()') -lt $launcher.IndexOf('fluxtop::download_release_update(*candidate)')) { throw 'Monitor closes before download' }
'Consent-only release update policy passed'