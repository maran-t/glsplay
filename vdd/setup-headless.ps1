<#
  One-shot headless-capture setup for glsplay on this GCP L4 box.
  Run once (self-elevates). Re-run only after editing this script.

  After this: reboot -> autologon -> glsplay-host starts itself. RDP in to
  peek, disconnect -> glsplay-reclaim hands the session back to the console
  and the (still-running) host re-acquires the display.

  The logon account is taken from the autologon config, not hardcoded -
  override with $env:GLSPLAY_LOGON_USER.
#>

# --- self-elevate ----------------------------------------------------------
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Start-Process powershell.exe "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
  exit
}

$ErrorActionPreference = 'Stop'
$repo = 'C:\glsplay'
$npm  = 'C:\Program Files\nodejs\npm.cmd'

function Ok($m){ Write-Host "  [ok] $m" -ForegroundColor Green }
function Info($m){ Write-Host "==> $m" -ForegroundColor Cyan }

# The account the glsplay-host task logs on as. Not hardcoded - it must match
# whoever autologon signs into the console. Order: explicit override, then the
# configured autologon user (Winlogon DefaultUserName), then the current
# interactive user.
function Resolve-LogonUser {
  if ($env:GLSPLAY_LOGON_USER) { return $env:GLSPLAY_LOGON_USER }

  $w = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' -ErrorAction SilentlyContinue
  if ($w.DefaultUserName) {
    if ($w.DefaultDomainName -and $w.DefaultDomainName -ne $env:COMPUTERNAME -and $w.DefaultDomainName -ne '.') {
      return "$($w.DefaultDomainName)\$($w.DefaultUserName)"
    }
    return $w.DefaultUserName
  }

  $ex = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($ex) {
    $o = Invoke-CimMethod -InputObject $ex -MethodName GetOwner -ErrorAction SilentlyContinue
    if ($o.User) { return $o.User }
  }
  return $env:USERNAME
}
$user = Resolve-LogonUser
Info "logon user for glsplay-host: $user  (override with `$env:GLSPLAY_LOGON_USER)"

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
# DISABLED: apps/signaling and apps/web moved to the separate glsplay-web repo,
# so the @glsplay/signaling / @glsplay/web workspaces no longer exist in this
# repo and `npm run start -w ...` would fail. Register these boot tasks from
# the glsplay-web repo instead (with its own checkout path).
# Info 'Signaling + web startup tasks'
# foreach ($svc in @(
#   @{ n='glsplay-signaling'; w='@glsplay/signaling' },
#   @{ n='glsplay-web';       w='@glsplay/web' })) {
#   $a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"cd /d $repo && `"$npm`" run start -w $($svc.w)`""
#   $trg = New-ScheduledTaskTrigger -AtStartup
#   $s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 5
#   Register-ScheduledTask -TaskName $svc.n -Action $a -Trigger $trg -Settings $s -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
#   Ok "task '$($svc.n)' at startup"
# }

# --- 4. host + console-reclaim tasks ----------------------------------
Info 'Host + console reclaim'

# host: starts at logon (autologon puts $user on the console after every reboot)
# and restarts if it exits. RunLevel Highest, no time limit. MultipleInstances
# IgnoreNew so a second logon / a reclaim `schtasks /run` never spawns a
# duplicate. reclaim-console.ps1 still runs it on demand as a safety net.
$hostAction  = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-ExecutionPolicy Bypass -File `"$repo\run-host.ps1`""
$hostTrigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$hostTrigger.Delay = 'PT15S'   # let the console session + VDD settle first
$hostSettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew `
  -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 3
Register-ScheduledTask -TaskName 'glsplay-host' -Action $hostAction -Trigger $hostTrigger `
  -Settings $hostSettings -User $user -RunLevel Highest -Force | Out-Null
Ok "task 'glsplay-host'  (runAs=$user, trigger=AtLogon +15s, restart 3x/1min)"

# reclaim: SYSTEM, on RDP disconnect -> tscon session to console, re-pin VDD,
# and only restart the host if it actually died (see reclaim-console.ps1).
New-StateTask 'glsplay-reclaim' "$repo\vdd\reclaim-console.ps1" 4 'SYSTEM'

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

# signaling + web now live in the separate web/signaling repo; probe them only
# to report whether they happen to be up on this box.
$sig = 'DOWN'
try { $sig = (Invoke-RestMethod http://localhost:8080/health -TimeoutSec 3).status } catch { }
Write-Host "signaling :8080 (separate repo): $sig"
$web = 'DOWN'
try { $web = (Invoke-WebRequest http://localhost:3000 -TimeoutSec 3 -UseBasicParsing).StatusCode } catch { }
Write-Host "web       :3000 (separate repo): $web"

# External IP from the GCE metadata server, so this isn't pinned to one address.
$ip = '<VM_EXTERNAL_IP>'
try {
  $ip = Invoke-RestMethod -Headers @{ 'Metadata-Flavor' = 'Google' } -TimeoutSec 3 `
    'http://metadata.google.internal/computeMetadata/v1/instance/network-interfaces/0/access-configs/0/external-ip'
} catch { }

Write-Host ''
Write-Host '-----------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ' Done. From now on:' -ForegroundColor White
Write-Host "   * Reboot -> autologon ($user) -> glsplay-host starts itself (+15s)" -ForegroundColor Gray
Write-Host '   * RDP in to peek, then disconnect -> glsplay-reclaim hands the'   -ForegroundColor Gray
Write-Host '     session back to the console and the running host re-acquires DXGI' -ForegroundColor Gray
Write-Host "   * Browser on laptop: http://$ip`:3000  (web/signaling: separate repo)" -ForegroundColor Gray
Write-Host '   * Logs (reconnect RDP pauses the stream):' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\reclaim-console.log -Tail 10' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\host.log.res' -ForegroundColor Gray
Write-Host '       Get-Content C:\glsplay\host.log -Tail 40' -ForegroundColor Gray
Write-Host '-----------------------------------------------------------' -ForegroundColor DarkGray
