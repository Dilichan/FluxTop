# Shared launcher helpers; dot-sourcing this file has no update/build side effects.
function ConvertTo-NativeArgument([string]$Value) {
    # Windows CRT argv quoting, including embedded quotes and trailing backslashes.
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-FluxProcess {
    param([string]$File, [string[]]$Arguments, [string]$Directory,
          [int]$TimeoutSeconds = 30)
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.Arguments = (($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $info.WorkingDirectory = $Directory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['GIT_TERMINAL_PROMPT'] = '0'
    $info.EnvironmentVariables['GCM_INTERACTIVE'] = 'Never'
    $info.EnvironmentVariables['GIT_SSH_COMMAND'] = 'ssh -oBatchMode=yes -oConnectTimeout=10 -oStrictHostKeyChecking=yes'
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start child process' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            # Terminate only the child started here and its descendants; no process-name matching.
            $kill = [System.Diagnostics.ProcessStartInfo]::new()
            $kill.FileName = Join-Path $env:SystemRoot 'System32/taskkill.exe'
            $kill.Arguments = "/PID $($process.Id) /T /F"
            $kill.UseShellExecute = $false
            $kill.CreateNoWindow = $true
            $kill.RedirectStandardOutput = $true
            $kill.RedirectStandardError = $true
            $killer = [System.Diagnostics.Process]::Start($kill)
            try { if (-not $killer.WaitForExit(5000)) { $killer.Kill() } } finally { $killer.Dispose() }
            if (-not $process.HasExited) { $process.Kill() }
            [void]$process.WaitForExit(5000)
        }
        $outText = if ($stdout.Wait(1000)) { $stdout.Result } else { '' }
        $errText = if ($stderr.Wait(1000)) { $stderr.Result } else { '' }
        return [pscustomobject]@{
            ExitCode = $(if ($timedOut) { -1 } else { $process.ExitCode })
            Out = $outText
            Error = $errText
            TimedOut = $timedOut
        }
    } finally { $process.Dispose() }
}

function Invoke-FluxGit([string]$Repository, [string[]]$GitArgs) {
    $git = (Get-Command git.exe -ErrorAction Stop).Source
    # Scope ownership trust to this invocation and this explicitly chosen checkout.
    Invoke-FluxProcess -File $git -Directory $Repository -Arguments (
        @('-c', "safe.directory=$Repository", '-c', 'core.hooksPath=NUL',
          '-c', 'merge.autoStash=false', '-c', 'core.fsmonitor=false',
          '-c', 'maintenance.auto=false', '-c', 'gc.auto=0',
          '-c', 'credential.interactive=false', '-c', 'http.lowSpeedLimit=1',
          '-c', 'http.lowSpeedTime=10') + $GitArgs)
}

function Get-FluxCheckout([string]$Repository) {
    $head = Invoke-FluxGit $Repository @('rev-parse', '--verify', 'HEAD')
    $status = Invoke-FluxGit $Repository @('status', '--porcelain', '--untracked-files=normal')
    [pscustomobject]@{
        Commit = $(if ($head.ExitCode -eq 0) { $head.Out.Trim() } else { '' })
        Dirty = ($status.ExitCode -ne 0 -or $status.Out.Length -gt 0)
    }
}

function Update-FluxCheckout([string]$Repository) {
    $state = Get-FluxCheckout $Repository
    if (-not $state.Commit) { return 'no-head' }
    if ($state.Dirty) { return 'local-changes' }
    $branch = Invoke-FluxGit $Repository @('symbolic-ref', '--quiet', '--short', 'HEAD')
    if ($branch.ExitCode -ne 0) { return 'detached-head' }
    foreach ($marker in @('MERGE_HEAD', 'CHERRY_PICK_HEAD', 'REVERT_HEAD', 'rebase-merge', 'rebase-apply', 'sequencer')) {
        $path = Invoke-FluxGit $Repository @('rev-parse', '--git-path', $marker)
        if ($path.ExitCode -ne 0) { return 'git-error' }
        $markerPath = $path.Out.Trim()
        if (-not [IO.Path]::IsPathRooted($markerPath)) { $markerPath = Join-Path $Repository $markerPath }
        if (Test-Path -LiteralPath $markerPath) { return 'operation-in-progress' }
    }
    $remote = Invoke-FluxGit $Repository @('for-each-ref', '--format=%(upstream:remotename)', '--', "refs/heads/$($branch.Out.Trim())")
    $remoteRef = Invoke-FluxGit $Repository @('for-each-ref', '--format=%(upstream:remoteref)', '--', "refs/heads/$($branch.Out.Trim())")
    if ($remote.ExitCode -ne 0 -or $remoteRef.ExitCode -ne 0) { return 'git-error' }
    $remoteName = $remote.Out.Trim()
    $ref = $remoteRef.Out.Trim()
    if (-not $remoteName -or $remoteName -eq '.' -or -not $ref.StartsWith('refs/heads/')) { return 'no-upstream' }
    $fetch = Invoke-FluxGit $Repository @('fetch', '--no-tags', '--no-recurse-submodules', '--', $remoteName, $ref)
    if ($fetch.ExitCode -ne 0) { return 'fetch-failed' }
    $target = Invoke-FluxGit $Repository @('rev-parse', '--verify', 'FETCH_HEAD^{commit}')
    if ($target.ExitCode -ne 0) { return 'git-error' }
    # Recheck after network access: never merge over concurrent edits or a branch switch.
    $after = Get-FluxCheckout $Repository
    $afterBranch = Invoke-FluxGit $Repository @('symbolic-ref', '--quiet', '--short', 'HEAD')
    if ($after.Dirty -or $after.Commit -ne $state.Commit -or $afterBranch.Out -ne $branch.Out) { return 'checkout-changed' }
    if ($target.Out.Trim() -eq $state.Commit) { return 'up-to-date' }
    $ancestor = Invoke-FluxGit $Repository @('merge-base', '--is-ancestor', $state.Commit, $target.Out.Trim())
    if ($ancestor.ExitCode -ne 0) { return 'local-commits-or-diverged' }
    $merge = Invoke-FluxGit $Repository @('merge', '--ff-only', '--no-edit', '--', $target.Out.Trim())
    if ($merge.ExitCode -ne 0) { return 'merge-failed' }
    return 'updated'
}

function Find-FluxBuildTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = ''
    if (Test-Path -LiteralPath $vswhere) {
        $found = Invoke-FluxProcess -File $vswhere -Arguments @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath') -Directory $PSScriptRoot
        if ($found.ExitCode -eq 0) { $installation = $found.Out.Trim() }
    }
    if ($installation) {
        $module = Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
        if (Test-Path -LiteralPath $module) {
            Import-Module $module -ErrorAction Stop
            Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        }
    }
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    $cmakePath = if ($cmake) { $cmake.Source } elseif ($installation) {
        Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    } else { '' }
    $ninjaPath = if ($ninja) { $ninja.Source } elseif ($installation) {
        Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    } else { '' }
    if (-not $cmakePath -or -not (Test-Path -LiteralPath $cmakePath)) { throw 'CMake not found; install C++ Build Tools and CMake' }
    [pscustomobject]@{ CMake = $cmakePath; Ninja = $(if ($ninjaPath -and (Test-Path -LiteralPath $ninjaPath)) { $ninjaPath } else { '' }) }
}

function Build-FluxVersion([string]$Repository, [string]$StateDirectory, [string]$Version) {
    $tools = Find-FluxBuildTools
    $build = Join-Path $StateDirectory "build/$Version"
    $configure = @('-S', $Repository, '-B', $build, '-DBUILD_TESTING=ON')
    if ($tools.Ninja) { $configure += @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)") }
    else { $configure += @('-A', 'x64') }
    $log = Join-Path $StateDirectory 'build.log'
    Set-Content -LiteralPath $log -Value '' -Encoding UTF8
    foreach ($arguments in @(
        @{ Args = $configure },
        @{ Args = @('--build', $build, '--config', 'Release') }
    )) {
        $result = Invoke-FluxProcess -File $tools.CMake -Arguments $arguments.Args -Directory $Repository -TimeoutSeconds 300
        Add-Content -LiteralPath $log -Value ($result.Out + $result.Error) -Encoding UTF8
        if ($result.ExitCode -ne 0) { throw 'Build failed; see out/launcher/build.log' }
    }
    $ctest = Join-Path (Split-Path $tools.CMake) 'ctest.exe'
    $test = Invoke-FluxProcess -File $ctest -Arguments @('--test-dir', $build, '-C', 'Release', '--output-on-failure') -Directory $Repository -TimeoutSeconds 120
    Add-Content -LiteralPath $log -Value ($test.Out + $test.Error) -Encoding UTF8
    if ($test.ExitCode -ne 0 -or $test.Out -notmatch '100% tests passed') { throw 'Tests failed or missing; see out/launcher/build.log' }
    $builtExe = if ($tools.Ninja) { Join-Path $build 'fluxtop.exe' } else { Join-Path $build 'Release/fluxtop.exe' }
    $destination = Join-Path $StateDirectory "versions/$Version"
    [void](New-Item -ItemType Directory -Path $destination -Force)
    $installed = Join-Path $destination 'fluxtop.exe'
    Copy-Item -LiteralPath $builtExe -Destination $installed -ErrorAction Stop
    return $installed
}

function Get-FluxLastGood([string]$StateDirectory) {
    $manifest = Join-Path $StateDirectory 'last-good.json'
    if (-not (Test-Path -LiteralPath $manifest)) { return $null }
    try {
        $value = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($value.Version -notmatch '^[a-zA-Z0-9-]+$') { return $null }
        $exe = Join-Path $StateDirectory "versions/$($value.Version)/fluxtop.exe"
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { return $null }
        return [pscustomobject]@{ Version = $value.Version; Exe = $exe }
    } catch { return $null }
}
