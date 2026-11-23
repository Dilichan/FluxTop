param([string]$CompilerPath,[string]$ProbePath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=$CompilerPath
$info.Arguments='/nologo /showIncludes /EP /TP "'+$ProbePath+'"'
$info.UseShellExecute=$false;$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
# cl uses Windows ANSI for redirected localized diagnostics; Ninja converts
# these to UTF-8. Avoid CMake's OEM decoding of that prefix.
$info.StandardOutputEncoding=[Text.Encoding]::Default
$info.StandardErrorEncoding=[Text.Encoding]::Default
$process=[Diagnostics.Process]::new();$process.StartInfo=$info
[void]$process.Start()
$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
if($process.ExitCode -ne 0){throw $output}
if($output -notmatch '(?m)^([^\r\n]+?)\s+[A-Za-z]:[^\r\n]*fluxtop-prefix-probe[.]hpp'){throw 'MSVC did not emit the probe header.'}
[Console]::Write($Matches[1].Trim())
$process.Dispose()
