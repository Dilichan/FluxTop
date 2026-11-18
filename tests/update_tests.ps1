$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/UpdateSupport.ps1')
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ('../build/update tests ' + [guid]::NewGuid().ToString('N'))))
[void](New-Item -ItemType Directory -Path $root)
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Git([string]$Directory, [string[]]$Arguments) {
    $result = Invoke-FluxGit $Directory (@('-c', 'user.name=FluxTop Tests', '-c', 'user.email=tests@example.invalid') + $Arguments)
    if ($result.ExitCode -ne 0) { throw "Fixture git failed: $($Arguments -join ' '): $($result.Error)" }
    return $result.Out.Trim()
}
function Commit-Note([string]$Directory, [string]$Value) {
    Set-Content -LiteralPath (Join-Path $Directory 'note.txt') -Value $Value
    $null = Git $Directory @('add', 'note.txt')
    $null = Git $Directory @('commit', '-m', $Value)
}
$remote = Join-Path $root 'remote.git'
$seed = Join-Path $root 'seed'
$client = Join-Path $root 'client'
$null = Git $root @('init', '--bare', '--initial-branch=main', $remote)
$null = Git $root @('init', '--initial-branch=main', $seed)
Assert ((Update-FluxCheckout $seed) -eq 'no-head') 'Unborn checkout must skip'
Commit-Note $seed 'first'
Assert ((Update-FluxCheckout $seed) -eq 'no-upstream') 'Missing upstream must skip'
$null = Git $seed @('remote', 'add', 'origin', $remote)
$null = Git $seed @('push', '-u', 'origin', 'main')
$null = Git $root @('clone', $remote, $client)
Assert ((Update-FluxCheckout $client) -eq 'up-to-date') 'Initial clone must be current'
Commit-Note $seed 'second'
$null = Git $seed @('push')
Assert ((Update-FluxCheckout $client) -eq 'updated') 'Remote fast-forward must install'
Assert ((Get-Content -LiteralPath (Join-Path $client 'note.txt')).Trim() -eq 'second') 'Updated file missing'
Assert ((Update-FluxCheckout $client) -eq 'up-to-date') 'Second update must be idempotent'

Set-Content -LiteralPath (Join-Path $client 'note.txt') -Value 'local edit'
$before = Git $client @('rev-parse', 'HEAD')
Assert ((Update-FluxCheckout $client) -eq 'local-changes') 'Tracked edit must block update'
Assert ((Git $client @('rev-parse', 'HEAD')) -eq $before) 'Dirty HEAD changed'
Assert ((Get-Content -LiteralPath (Join-Path $client 'note.txt')).Trim() -eq 'local edit') 'Local edit lost'
$null = Git $client @('restore', 'note.txt') # Only restore the fixture edit created above.
$untracked = Join-Path $client 'untracked.txt'
Set-Content -LiteralPath $untracked -Value 'keep'
Assert ((Update-FluxCheckout $client) -eq 'local-changes') 'Untracked file must block update'
Remove-Item -LiteralPath $untracked

$null = Git $client @('checkout', '--detach')
Assert ((Update-FluxCheckout $client) -eq 'detached-head') 'Detached checkout must skip'
$null = Git $client @('checkout', 'main')
$gitDir = Git $client @('rev-parse', '--absolute-git-dir')
$mergeMarker = Join-Path $gitDir 'MERGE_HEAD'
Set-Content -LiteralPath $mergeMarker -Value $before
Assert ((Update-FluxCheckout $client) -eq 'operation-in-progress') 'Pending merge must skip'
Remove-Item -LiteralPath $mergeMarker

Commit-Note $client 'local commit'
Assert ((Update-FluxCheckout $client) -eq 'local-commits-or-diverged') 'Local ahead must not merge'
Commit-Note $seed 'remote third'
$null = Git $seed @('push')
Assert ((Update-FluxCheckout $client) -eq 'local-commits-or-diverged') 'Divergence must not merge'
Assert ((Get-Content -LiteralPath (Join-Path $client 'note.txt')).Trim() -eq 'local commit') 'Divergent content changed'
$null = Git $client @('remote', 'set-url', 'origin', (Join-Path $root 'missing.git'))
Assert ((Update-FluxCheckout $client) -eq 'fetch-failed') 'Failed fetch must stay local'

# Exercise native argv quoting without a shell: spaces, quotes and trailing backslash.
$powershell = (Get-Command powershell.exe).Source
$echoScript = Join-Path $root 'echo args.ps1'
Set-Content -LiteralPath $echoScript -Value '[Console]::Write($args[0])' -Encoding UTF8
$value = 'space "quote" and trailing\'
$echo = Invoke-FluxProcess -File $powershell -Directory $root -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $echoScript, $value)
Assert ($echo.ExitCode -eq 0 -and $echo.Out -eq $value) 'Native argument roundtrip failed'
$timeout = Invoke-FluxProcess -File $powershell -Directory $root -Arguments @('-NoProfile', '-Command', 'Start-Sleep -Seconds 10') -TimeoutSeconds 1
Assert ($timeout.TimedOut -and $timeout.ExitCode -eq -1) 'Timeout was not enforced'

$state = Join-Path $root 'state'
[void](New-Item -ItemType Directory -Path $state)
@{ Version = '../../outside' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $state 'last-good.json')
Assert ($null -eq (Get-FluxLastGood $state)) 'Invalid last-good path accepted'
'Git updater checks passed'
