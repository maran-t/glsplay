<#
.SYNOPSIS
  Verifies a VM is ready to run the glsplay host, before you waste time
  debugging the daemon.

.DESCRIPTION
  Every check here corresponds to a failure that is painful to diagnose from
  the host's own logs. Run it on the VM after setup and after every reboot.

  Exit code 0 means ready. 1 means at least one blocking problem.
#>

[CmdletBinding()]
param(
  [int]$MediaPortStart = 50000,
  [int]$MediaPortEnd = 50100
)

$script:Failures = 0
$script:Warnings = 0

function Write-Head { param([string]$m) Write-Host "`n$m" -ForegroundColor White; Write-Host ('-' * 61) -ForegroundColor DarkGray }
function Pass { param([string]$m) Write-Host '  PASS ' -ForegroundColor Green -NoNewline; Write-Host $m }
function Fail { param([string]$m, [string]$fix) Write-Host '  FAIL ' -ForegroundColor Red -NoNewline; Write-Host $m; if ($fix) { Write-Host "       -> $fix" -ForegroundColor DarkGray }; $script:Failures++ }
function Warn { param([string]$m, [string]$fix) Write-Host '  WARN ' -ForegroundColor Yellow -NoNewline; Write-Host $m; if ($fix) { Write-Host "       -> $fix" -ForegroundColor DarkGray }; $script:Warnings++ }
function Info { param([string]$m) Write-Host '       ' -NoNewline; Write-Host $m -ForegroundColor DarkGray }

Write-Host ''
Write-Host ' glsplay host environment check' -ForegroundColor Cyan
Write-Host " $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $env:COMPUTERNAME" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
Write-Head '1. Session - the single most common cause of a black stream'

# Desktop Duplication captures the session that owns the display. In an RDP
# session the display is the RDP virtual driver, which has no NVENC, so
# everything appears to work until you get zero frames or a software encoder.
$currentSession = (Get-Process -Id $PID).SessionId

# WTSGetActiveConsoleSessionId is the authoritative source. query.exe is not
# always resolvable depending on how the shell was launched, and parsing its
# output is fragile across locales.
$consoleSessionId = $null
try {
  if (-not ('Glsplay.NativeSession' -as [type])) {
    Add-Type -Namespace 'Glsplay' -Name 'NativeSession' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint WTSGetActiveConsoleSessionId();
'@
  }
  $consoleSessionId = [int][Glsplay.NativeSession]::WTSGetActiveConsoleSessionId()
} catch {
  Info "console session lookup failed: $($_.Exception.Message)"
}

# SESSIONNAME is "Console" at the console and "RDP-Tcp#N" over Remote Desktop.
$sessionName = $env:SESSIONNAME
Info "session $currentSession, SESSIONNAME=$(if ($sessionName) { $sessionName } else { '(unset)' })"
if ($null -ne $consoleSessionId) { Info "console session is $consoleSessionId" }

if ($sessionName -match '^RDP-Tcp') {
  Fail 'running in an RDP session - Desktop Duplication captures the RDP display, which has no NVENC' `
       'Disconnect RDP, then from an elevated prompt: tscon <id> /dest:console. Or use the GCP serial console for setup.'
} elseif ($null -ne $consoleSessionId -and $currentSession -eq $consoleSessionId) {
  Pass 'running in the CONSOLE session'
} elseif ($sessionName -eq 'Console') {
  Pass 'SESSIONNAME reports Console'
} else {
  Warn "session $currentSession is not the console session ($consoleSessionId)" `
       'The host must run at the console. Run: tscon <id> /dest:console'
}

# ---------------------------------------------------------------------------
Write-Head '2. GPU and NVENC'

$nvidia = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' }
if ($nvidia) {
  Pass "$($nvidia.Name)"
  Info "driver $($nvidia.DriverVersion)"
  if ($nvidia.Name -notmatch 'L4') {
    Warn "expected an L4; found '$($nvidia.Name)'" 'Fine for local testing, but not the target hardware.'
  }
} else {
  Fail 'no NVIDIA adapter found' 'Run setup-gcp-vm.ps1, or install the L4 data-center driver manually.'
}

if (Test-Path "$env:SystemRoot\System32\nvEncodeAPI64.dll") {
  Pass 'nvEncodeAPI64.dll present'
} else {
  Fail 'nvEncodeAPI64.dll missing - NVENC unavailable' 'Install the NVIDIA driver. NVENC ships in the driver, not the SDK.'
}

if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
  $smi = & nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>$null
  if ($smi) { Info "nvidia-smi: $smi" }
  # A GPU in WDDM mode can drive a display; TCC mode cannot, and Desktop
  # Duplication would then have nothing to bind to.
  $mode = & nvidia-smi --query-gpu=driver_model.current --format=csv,noheader 2>$null
  if ($mode) {
    if ($mode -match 'WDDM') { Pass "driver model is WDDM" }
    else { Fail "driver model is $mode - Desktop Duplication requires WDDM" 'Switch with: nvidia-smi -dm 0   then reboot.' }
  }
} else {
  Warn 'nvidia-smi not on PATH' 'Usually at C:\Program Files\NVIDIA Corporation\NVSMI'
}

# ---------------------------------------------------------------------------
Write-Head '3. Display - the L4 has no head of its own'

$adapters = Get-CimInstance Win32_VideoController
foreach ($a in $adapters) { Info "adapter: $($a.Name)  [$($a.VideoModeDescription)]" }

# An IDD virtual display presents as a monitor with no physical connection.
$monitors = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams -ErrorAction SilentlyContinue
$monitorCount = ($monitors | Measure-Object).Count
if ($monitorCount -ge 1) {
  Pass "$monitorCount display(s) attached"
} else {
  Fail 'no display attached - DXGI has nothing to duplicate' 'Run install-virtual-display.ps1 to install the IDD virtual display driver.'
}

Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
$screens = [System.Windows.Forms.Screen]::AllScreens
foreach ($s in $screens) {
  Info "screen $($s.DeviceName): $($s.Bounds.Width)x$($s.Bounds.Height) primary=$($s.Primary)"
}
$primary = [System.Windows.Forms.Screen]::PrimaryScreen
if ($primary) {
  if ($primary.Bounds.Width -ge 1920 -and $primary.Bounds.Height -ge 1080) {
    Pass "primary display is $($primary.Bounds.Width)x$($primary.Bounds.Height)"
  } else {
    Warn "primary display is only $($primary.Bounds.Width)x$($primary.Bounds.Height)" `
         'PRD section 4.1 targets 1920x1080. Add the mode to the IDD driver and select it in Display Settings.'
  }
}

# Refresh rate must be at least the target frame rate or capture is capped.
$refresh = (Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentRefreshRate } |
            Select-Object -First 1).CurrentRefreshRate
if ($refresh) {
  if ($refresh -ge 60) { Pass "refresh rate ${refresh}Hz" }
  else { Fail "refresh rate is only ${refresh}Hz" 'Capture cannot exceed the refresh rate. Select a 60Hz mode.' }
}

# ---------------------------------------------------------------------------
Write-Head '4. Audio'

$audioSvc = Get-Service -Name Audiosrv -ErrorAction SilentlyContinue
if ($audioSvc -and $audioSvc.Status -eq 'Running') {
  Pass 'Windows Audio service running'
} else {
  Warn 'Windows Audio service not running' 'Set-Service Audiosrv -StartupType Automatic; Start-Service Audiosrv'
}

$sound = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue
if ($sound) {
  Pass "audio endpoint: $($sound[0].Name)"
} else {
  Warn 'no audio endpoint - WASAPI loopback has nothing to capture' `
       'Install a virtual audio device, or run the host with --no-audio (PRD phase 3 will be skipped).'
}

# ---------------------------------------------------------------------------
Write-Head '5. Firewall'

$mediaRule = Get-NetFirewallRule -DisplayName 'glsplay*' -ErrorAction SilentlyContinue
if ($mediaRule) {
  Pass "$(($mediaRule | Measure-Object).Count) glsplay firewall rule(s) present"
  foreach ($r in $mediaRule) { Info "$($r.DisplayName)  enabled=$($r.Enabled)  action=$($r.Action)" }
} else {
  Fail 'no glsplay firewall rules found' 'Run setup-firewall.ps1 (and confirm the GCP VPC rules exist too).'
}

Info "Remember the VPC rule as well - Windows Firewall and GCP firewall are separate layers."
Info "Media range expected: UDP $MediaPortStart-$MediaPortEnd"

# ---------------------------------------------------------------------------
Write-Head '6. Gamepad (PRD phase 5 only)'

$vigem = Get-CimInstance Win32_SystemDriver -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match 'ViGEm' }
if ($vigem) {
  Pass "ViGEmBus present (state: $($vigem.State))"
} else {
  Warn 'ViGEmBus not installed - gamepad injection unavailable' `
       'Only needed for PRD phase 5. The host runs fine without it (NullGamepadSink).'
}

# Secure Boot blocks unsigned kernel drivers. ViGEmBus is signed, so this is
# informational, but it explains a failed driver load if one happens.
try {
  $secureBoot = Confirm-SecureBootUEFI -ErrorAction Stop
  Info "Secure Boot: $secureBoot"
} catch {
  Info 'Secure Boot: not reported (likely BIOS boot)'
}

# ---------------------------------------------------------------------------
Write-Head '7. Session hygiene'

$winlogon = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' -ErrorAction SilentlyContinue
if ($winlogon.AutoAdminLogon -eq '1') {
  Pass "auto-logon enabled for $($winlogon.DefaultUserName)"
} else {
  Warn 'auto-logon not enabled' 'After a reboot there will be no console session, so nothing to capture.'
}

$lock = Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' -Name NoLockScreen -ErrorAction SilentlyContinue
if ($lock.NoLockScreen -eq 1) { Pass 'lock screen disabled' }
else { Warn 'lock screen not disabled' 'A locked session shows the secure desktop, which cannot be duplicated.' }

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host ('=' * 61) -ForegroundColor DarkGray
if ($script:Failures -eq 0 -and $script:Warnings -eq 0) {
  Write-Host ' READY - no problems found' -ForegroundColor Green
} elseif ($script:Failures -eq 0) {
  Write-Host " READY with $($script:Warnings) warning(s)" -ForegroundColor Yellow
  Write-Host ' Warnings are non-blocking; the affected phase will not work.' -ForegroundColor DarkGray
} else {
  Write-Host " NOT READY - $($script:Failures) failure(s), $($script:Warnings) warning(s)" -ForegroundColor Red
  Write-Host ' Fix the failures above before running glsplay-host.' -ForegroundColor DarkGray
}
Write-Host ('=' * 61) -ForegroundColor DarkGray
Write-Host ''

exit ($(if ($script:Failures -gt 0) { 1 } else { 0 }))
