[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$Executable,
 [string]$OutputDirectory=(Join-Path $PSScriptRoot ('../build/profiles/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))),
 [ValidateSet('CLI','Overlay','Hidden','Settings','Statistics','Detached')][string[]]$Modes=@('CLI','Overlay','Hidden','Settings','Statistics','Detached'),
 [ValidateRange(250,60000)][int[]]$Intervals=@(250,500,1000),
 [ValidateRange(10,86400)][int]$DurationSeconds=60,
 [ValidateRange(5,3600)][int]$WarmupSeconds=30,
 [ValidateRange(1,20)][int]$Repetitions=1
)
$ErrorActionPreference='Stop'
if(-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Profiling requires Administrator.'}
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $OutputDirectory) -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count){throw 'Choose an empty output directory so results and fixture history cannot mix.'}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
if(-not ('FluxTopProfileNative' -as [type])){Add-Type -TypeDefinition @"
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public static class FluxTopProfileNative {
 delegate bool EnumProc(IntPtr w,IntPtr p);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc f,IntPtr p);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr w,EnumProc f,IntPtr p);
 [DllImport("user32.dll")]static extern int GetClassName(IntPtr w,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll")]static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint f,uint ms,out IntPtr r);
 [DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")]public static extern int GetDlgCtrlID(IntPtr w);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr w);
 [DllImport("user32.dll")]public static extern bool ShowWindowAsync(IntPtr w,int command);
 public struct Rect {public int left,top,right,bottom;}
 public static IntPtr Find(uint owner,string cls,IntPtr parent,int id=-1){IntPtr answer=IntPtr.Zero;EnumProc f=(w,p)=>{uint process;GetWindowThreadProcessId(w,out process);var s=new StringBuilder(256);GetClassName(w,s,256);if(process==owner && String.Equals(s.ToString(),cls,StringComparison.OrdinalIgnoreCase) && (id<0 || GetDlgCtrlID(w)==id)){answer=w;return false;}return true;};if(parent==IntPtr.Zero)EnumWindows(f,IntPtr.Zero);else EnumChildWindows(parent,f,IntPtr.Zero);return answer;}
 public static void Send(IntPtr w,uint message,int first,int second=0){if(w==IntPtr.Zero)throw new Exception("Missing owned profiling window");IntPtr result;if(SendMessageTimeout(w,message,new IntPtr(first),new IntPtr(second),2,5000,out result)==IntPtr.Zero)throw new Exception("Owned profiling window did not respond");}
 public static int Count(uint owner,string cls){int count=0;EnumWindows((w,p)=>{uint process;GetWindowThreadProcessId(w,out process);var s=new StringBuilder(256);GetClassName(w,s,256);if(process==owner && s.ToString()==cls)++count;return true;},IntPtr.Zero);return count;}
}
"@}
function Find-Owned([uint32]$Owner,[string]$Class,[IntPtr]$Parent=[IntPtr]::Zero,[int]$Control=-1){[FluxTopProfileNative]::Find($Owner,$Class,$Parent,$Control)}
function Distribution([double[]]$Values){$v=@($Values | Sort-Object);if(-not $v.Count){return $null};[ordered]@{mean=($v|Measure-Object -Average).Average;p50=$v[[Math]::Ceiling($v.Count*.50)-1];p95=$v[[Math]::Ceiling($v.Count*.95)-1];p99=$v[[Math]::Ceiling($v.Count*.99)-1];max=$v[-1]}}
$metadata=[ordered]@{started_utc=[DateTime]::UtcNow.ToString('o');executable=$Executable;sha256=(Get-FileHash -LiteralPath $Executable).Hash;windows=(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,TotalVisibleMemorySize);cpu=(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);gpu=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);duration_seconds=$DurationSeconds;warmup_seconds=$WarmupSeconds;intervals=$Intervals;modes=$Modes;note='External process CPU, including its background workers; system ETW/WMI costs are separate. Collection latency from diagnostics/CLI. No heap instrumentation in this executable.'}
$metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDirectory 'environment.json') -Encoding UTF8
$results=[Collections.Generic.List[object]]::new()
foreach($round in 1..$Repetitions){foreach($interval in $Intervals){foreach($mode in $Modes){
 $name="$mode-$interval-$round";$folder=Join-Path $OutputDirectory $name;[void][IO.Directory]::CreateDirectory($folder)
 $env:FLUXTOP_DATA_DB=Join-Path $folder 'history.db';$env:FLUXTOP_LOG_PATH=Join-Path $folder 'activity.log';$env:FLUXTOP_OVERLAY_SETTINGS=Join-Path $folder 'settings.ini'
 $env:FLUXTOP_OVERLAY_INSPECT='0'
 # Explicit settings are private, identical across builds and never replace the user's INI.
 [IO.File]::WriteAllText($env:FLUXTOP_OVERLAY_SETTINGS,"[overlay]`r`nlayout_version=3`r`nmetrics=31`r`ntop=10`r`nstats_selected_section=0`r`n")
 $arguments="--interval-ms $interval --top 10 --samples 100000"
 if($mode -eq 'CLI'){$arguments+=' --jsonl'}else{$arguments+=' --overlay --tray'}
 $savedLocalAppData=$env:LOCALAPPDATA
 try {
  # Prevent the profiling client from starting an installed release launcher.
  $env:LOCALAPPDATA=Join-Path $folder 'local-appdata'
  [void][IO.Directory]::CreateDirectory($env:LOCALAPPDATA)
  $child=Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'stdout.jsonl') -RedirectStandardError (Join-Path $folder 'stderr.log') -PassThru
 } finally {$env:LOCALAPPDATA=$savedLocalAppData}
 try {
  $overlay=[IntPtr]::Zero;$client=[IntPtr]::Zero;[uint32]$uiThread=0
  if($mode -ne 'CLI'){
   for($i=0;$i -lt 100 -and $overlay -eq [IntPtr]::Zero;$i++){Start-Sleep -Milliseconds 100;$overlay=Find-Owned $child.Id 'FluxTop.Overlay.Window'}
   if($overlay -eq [IntPtr]::Zero){throw 'Owned overlay did not start'}
   [uint32]$owner=0;$uiThread=[FluxTopProfileNative]::GetWindowThreadProcessId($overlay,[ref]$owner)
   if($mode -eq 'Hidden'){[FluxTopProfileNative]::Send($overlay,0x312,1)}
   if($mode -in @('Settings','Statistics','Detached')){
    [FluxTopProfileNative]::Send($overlay,0x312,2);$client=Find-Owned $child.Id 'FluxTop.Client.Window'
    if($client -eq [IntPtr]::Zero){throw 'Owned client did not open'}
    [void][FluxTopProfileNative]::ShowWindowAsync($client,5)
    if($mode -in @('Statistics','Detached')){
     $tab=Find-Owned $child.Id 'SysTabControl32' $client 450
     # TCM_SETCURFOCUS changes selection and notifies the tab's parent.
     [FluxTopProfileNative]::Send($tab,0x1330,1)
     $page=[IntPtr]::Zero
     for($attempt=0;$attempt -lt 50;$attempt++){
      $page=Find-Owned $child.Id 'FluxTop.Statistics.Page' $client
      if($page -ne [IntPtr]::Zero -and [FluxTopProfileNative]::IsWindowVisible($page)){break}
      Start-Sleep -Milliseconds 100
     }
     if($page -eq [IntPtr]::Zero -or -not [FluxTopProfileNative]::IsWindowVisible($page)){throw "Statistics tab activation failed (client visible: $([FluxTopProfileNative]::IsWindowVisible($client)), page: $page)"}
     if($mode -eq 'Detached'){
      [FluxTopProfileNative]::Send($page,0x111,1212)
      $rect=New-Object FluxTopProfileNative+Rect;[void][FluxTopProfileNative]::GetClientRect($page,[ref]$rect)
      $x=[int](100*$rect.right/1400);$y=[int](60*$rect.bottom/940)
      [FluxTopProfileNative]::Send($page,0x202,0,($x -bor ($y -shl 16)))
      [FluxTopProfileNative]::Send($page,0x111,1007)
      if([FluxTopProfileNative]::Count($child.Id,'FluxTop.Statistics.DetachedGraph.v1') -lt 2){throw 'Two detached graphs did not open'}
     }
    }
   }
  }
  Start-Sleep -Seconds $WarmupSeconds
  if($child.HasExited){throw "Profile child exited: $($child.ExitCode)"}
  $child.Refresh();$startCpu=$child.TotalProcessorTime.TotalMilliseconds;$started=[DateTimeOffset]::UtcNow;$start=[Diagnostics.Stopwatch]::StartNew();$lastCpu=$startCpu;$lastTime=0.0
  $samples=[Collections.Generic.List[object]]::new()
  $uiStart=0.0;if($uiThread){foreach($t in $child.Threads){if($t.Id -eq $uiThread){$uiStart=$t.TotalProcessorTime.TotalMilliseconds}}}
  while($start.Elapsed.TotalSeconds -lt $DurationSeconds){
   Start-Sleep -Milliseconds 1000;$child.Refresh();if($child.HasExited){throw "Profile child exited: $($child.ExitCode)"}
   $time=$start.Elapsed.TotalMilliseconds;$current=$child.TotalProcessorTime.TotalMilliseconds
   $samples.Add([pscustomobject]@{elapsed_ms=$time;cpu_percent=100*($current-$lastCpu)/(($time-$lastTime)*[Environment]::ProcessorCount);working_set=$child.WorkingSet64;private_bytes=$child.PrivateMemorySize64;handles=$child.HandleCount;threads=$child.Threads.Count})
   $lastCpu=$current;$lastTime=$time
  }
  $uiEnd=$uiStart;if($uiThread){foreach($t in $child.Threads){if($t.Id -eq $uiThread){$uiEnd=$t.TotalProcessorTime.TotalMilliseconds}}}
  $samples | Export-Csv -LiteralPath (Join-Path $folder 'resources.csv') -NoTypeInformation -Encoding UTF8
  $ended=$started.AddMilliseconds($lastTime)
  $latencies=@(Get-Content $env:FLUXTOP_LOG_PATH | ForEach-Object {
   if($_ -match '^(\S+) .*sampler\.snapshot.*collection_ms=([0-9.]+)'){
    $timestamp=[DateTimeOffset]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
    if($timestamp -ge $started -and $timestamp -le $ended){[double]$Matches[2]}
   }
  })
  $result=[ordered]@{scenario=$name;cpu_average_percent=100*($lastCpu-$startCpu)/($lastTime*[Environment]::ProcessorCount);cpu=Distribution $samples.cpu_percent;working_set=Distribution $samples.working_set;private_bytes=Distribution $samples.private_bytes;collection_ms=Distribution $latencies;ui_cpu_ms=$uiEnd-$uiStart;handles_first=$samples[0].handles;handles_last=$samples[-1].handles;threads_first=$samples[0].threads;threads_last=$samples[-1].threads;measured_seconds=$lastTime/1000}
  $results.Add([pscustomobject]$result);$results | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $OutputDirectory 'results.json') -Encoding UTF8
 } finally {
  if(-not $child.HasExited){
   if($overlay -ne [IntPtr]::Zero){try{[FluxTopProfileNative]::Send($overlay,0x10,0)}catch{if(-not $child.HasExited){Write-Warning $_}}}
   if(-not $child.HasExited -and ($overlay -eq [IntPtr]::Zero -or -not $child.WaitForExit(5000))){$child.Kill();$child.WaitForExit()}
  }
  $child.Dispose()
 }
 Write-Host "$name complete"
}}}
