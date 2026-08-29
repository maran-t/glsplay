<#
  One-shot headless-capture setup for glsplay on this GCP L4 box.
  Run once (self-elevates). After this, the only recurring action is:
  disconnect RDP  ->  stream runs ;  reconnect RDP  ->  stream pauses.
#>

# --- self-elevate ----------------------------------------------------------
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Start-Process powershell.exe "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
  exit
}

$ErrorActionPreference = 'Stop'
$repo = 'C:\glsplay'
$user = 'maranmani_t99'
$npm  = 'C:\Program Files\nodejs\npm.cmd'

function Ok($m){ Write-Host "  [ok] $m" -ForegroundColor Green }
function Info($m){ Write-Host "==> $m" -ForegroundColor Cyan }

# --- 1. room secret as a machine env var ---------------------------------
Info 'Room secret'
$sec = ((Get-Content "$repo\.env" | Where-Object { $_ -match '^GLSPLAY_ROOM_SECRET=' }) -replace '^GLSPLAY_ROOM_SECRET=','').Trim()
if ($sec.Length -ne 64) { Write-Warning "secret length $($sec.Length), expected 64 - check .env" }
[Environment]::SetEnvironmentVariable('GLSPLAY_ROOM_SECRET', $sec, 'Machine')
Ok "GLSPLAY_ROOM_SECRET set for the machine ($($sec.Length) chars)"

# --- 2. helper to (re)register a session-state task ---------------------
function New-StateTask($name, $file, $stateChange, $runAs, $args='') {
  $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-ExecutionPolicy Bypass -File `"$file`" $args"
  $s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
  $t = $null
  if ($stateChange) {
    $t = New-CimInstance -CimClass (Get-CimClass MSFT_TaskSessionStateChangeTrigger root/Microsoft/Windows/TaskScheduler) -ClientOnly -Property @{ StateChange = $stateChange }
  }
  if ($t) {
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Settings $s -User $runAs -RunLevel Highest -Force | Out-Null
  } else {
    Register-ScheduledTask -TaskName $name -Action $a -Settings $s -User $runAs -RunLevel Highest -Force | Out-Null
  }
  Ok "task '$name'  (runAs=$runAs, trigger=$(if($stateChange){"stateChange $stateChange"}else{'on-demand'}))"
}

# --- 3. signaling + web at boot (so a reboot doesn't need you) ---------
Info 'Signaling + web startup tasks'
foreach ($svc in @(
  @{ n='glsplay-signaling'; w='@glsplay/signaling' },
  @{ n='glsplay-web';       w='@glsplay/web' })) {
  $a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"cd /d $repo && `"$npm`" run start -w $($svc.w)`""
  $trg = New-ScheduledTaskTrigger -AtStartup
  $s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 5
  Register-ScheduledTask -TaskName $svc.n -Action $a -Trigger $trg -Settings $s -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
  Ok "task '$($svc.n)' at startup"
}

# --- 4. host + console-reclaim + display tasks -----------------------
Info 'Host + console reclaim + display'
# host: on-demand only; reclaim-console.ps1 fires it after tscon
New-StateTask 'glsplay-host' "$repo\run-host.ps1" $null $user
# reclaim: SYSTEM, on RDP disconnect -> tscon session to console -> run host
New-StateTask 'glsplay-reclaim' "$repo\vdd\reclaim-console.ps1" 4 'SYSTEM'
# display: AS THE USER, on ConsoleConnect (which tscon produces) -> make the
# MTT virtual display the sole primary at 1920x1080. Runs in the console
# session's own context, the only place EnumDisplayDevices sees the displays.
$da = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument (
  "-ExecutionPolicy Bypass -Command ""& '$repo\vdd\set-vdd-res.ps1' *>&1 | " +
  "Out-File '$repo\set-vdd-res.log' -Append -Encoding utf8""")
$dt = New-CimInstance -CimClass (Get-CimClass MSFT_TaskSessionStateChangeTrigger root/Microsoft/Windows/TaskScheduler) -ClientOnly -Property @{ StateChange = 1 }  # ConsoleConnect
$ds = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'glsplay-display' -Action $da -Trigger $dt -Settings $ds -User $user -RunLevel Highest -Force | Out-Null
Ok "task 'glsplay-display' (runAs=$user, trigger=ConsoleConnect)"

# --- 5. lock / power hardening (idempotent) ---------------------------
Info 'Lock / power'
$sys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty $sys PromptOnSecureDesktop -Value 0 -Type DWord
Set-ItemProperty $sys InactivityTimeoutSecs -Value 0 -Type DWord
Set-ItemProperty $sys DisableLockWorkstation -Value 1 -Type DWord
$per = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization'
New-Item $per -Force | Out-Null
Set-ItemProperty $per NoLockScreen -Value 1 -Type DWord
powercfg /change monitor-timeout-ac 0 | Out-Null
powercfg /change standby-timeout-ac 0 | Out-Null
Ok 'secure desktop off, no auto-lock, no sleep'

# --- 6. status --------------------------------------------------------
Info 'Status'
Get-ScheduledTask glsplay-* | Select-Object TaskName, State | Format-Table -AutoSize
$sig = 'DOWN'
try { $sig = (Invoke-RestMethod http://localhost:8080/health -TimeoutSec 3).status } catch { }
Write-Host "signaling: $sig"
$web = 'DOWN'
try { $web = (Invoke-WebRequest http://localhost:3000 -TimeoutSec 3 -UseBasicParsing).StatusCode } catch { }
Write-Host "web 3000 : $web"

Write-Host ''
Write-Host '-----------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ' Done. From now on:' -ForegroundColor White
Write-Host '   * Disconnect RDP (X, not Sign out)  -> host starts, stream works' -ForegroundColor Gray
Write-Host '   * Browser on laptop: http://34.180.13.189:3000' -ForegroundColor Gray
Write-Host '   * Reconnect RDP only to check logs (it pauses the stream):' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\reclaim-console.log' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\host.log.res' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\host.log -Tail 40' -ForegroundColor Gray
Write-Host '-----------------------------------------------------------' -ForegroundColor DarkGray
