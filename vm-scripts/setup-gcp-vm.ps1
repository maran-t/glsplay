<#
.SYNOPSIS
  Prepares a fresh GCP Windows Server 2022 VM to run the glsplay host.

.DESCRIPTION
  Run this ON THE VM, as Administrator, once. It installs the NVIDIA driver,
  enables the services the host depends on, configures auto-logon so a console
  session always exists, and disables the power and visual settings that
  interfere with capture.

  Reboot afterwards, then run check-environment.ps1 to confirm.

.NOTES
  Auto-logon is the part that surprises people. Desktop Duplication can only
  capture a session that owns the display. If nobody is logged in at the
  console, there is no desktop to duplicate and the host has nothing to send.
#>

[CmdletBinding()]
param(
  # Auto-logon account. Defaults to the account running this script - it is not
  # pinned to any particular name. Only the password has to be supplied (there
  # is no way to derive it); pass -AutoLogonPassword to (re)configure auto-logon.
  [string]$AutoLogonUser = $env:USERNAME,
  [string]$AutoLogonPassword,

  [switch]$SkipDriver
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
      ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script as Administrator.'
}

function Write-Step { param([string]$m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "    [!!] $m" -ForegroundColor Yellow }

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$downloadDir = Join-Path $env:TEMP 'glsplay-setup'
New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null

# --- 1. NVIDIA driver -------------------------------------------------------

Write-Step 'NVIDIA driver'

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' }
if ($gpu) {
  Write-Ok "$($gpu.Name) driver $($gpu.DriverVersion)"
} elseif ($SkipDriver) {
  Write-Warn 'No NVIDIA adapter and -SkipDriver was passed; continuing'
} else {
  Write-Warn 'No NVIDIA adapter detected yet - installing the GCP driver package'
  # Google publishes a driver installer that picks the right package for the
  # attached GPU. Using it avoids guessing a version that does not match L4.
  $installer = Join-Path $downloadDir 'install_gpu_driver.ps1'
  try {
    Invoke-WebRequest -Uri 'https://github.com/GoogleCloudPlatform/compute-gpu-installation/releases/latest/download/install_gpu_driver.ps1' `
      -OutFile $installer -UseBasicParsing
    Write-Ok 'downloaded Google GPU driver installer'
    Write-Host '    running installer - this takes several minutes and may reboot' -ForegroundColor DarkGray
    & powershell -ExecutionPolicy Bypass -File $installer
  } catch {
    Write-Warn "automatic driver install failed: $($_.Exception.Message)"
    Write-Warn 'Install the L4 driver manually from https://www.nvidia.com/download/index.aspx'
    Write-Warn 'Choose the Data Center / Tesla branch for L4 on Windows Server 2022.'
  }
}

# NVENC lives in the driver, not the SDK. If this is missing after a driver
# install, the wrong driver variant went on.
if (Test-Path "$env:SystemRoot\System32\nvEncodeAPI64.dll") {
  Write-Ok 'nvEncodeAPI64.dll present - NVENC available'
} else {
  Write-Warn 'nvEncodeAPI64.dll NOT found - NVENC will not work until the driver is installed'
}

# --- 2. Services ------------------------------------------------------------

Write-Step 'Services'

# Windows Audio is disabled by default on Server SKUs. WASAPI loopback has
# nothing to attach to without it, so PRD phase 3 fails with a confusing
# "no endpoint" error rather than anything about the service.
foreach ($svc in @('Audiosrv', 'AudioEndpointBuilder')) {
  try {
    Set-Service -Name $svc -StartupType Automatic -ErrorAction Stop
    Start-Service -Name $svc -ErrorAction SilentlyContinue
    Write-Ok "$svc set to Automatic and started"
  } catch {
    Write-Warn "could not configure ${svc}: $($_.Exception.Message)"
  }
}

# Even with the service running, a Server VM usually has no audio endpoint at
# all. A virtual audio device is required for real loopback capture.
$endpoints = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue
if (-not $endpoints) {
  Write-Warn 'No audio endpoint present. Install a virtual audio device (for example'
  Write-Warn 'VB-CABLE) before testing PRD phase 3, or run the host with --no-audio.'
} else {
  Write-Ok "audio endpoint: $($endpoints[0].Name)"
}

# --- 3. Auto-logon ----------------------------------------------------------

Write-Step 'Console session'

$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
$existing = Get-ItemProperty -Path $winlogon -ErrorAction SilentlyContinue

if ($AutoLogonPassword) {
  Set-ItemProperty -Path $winlogon -Name 'AutoAdminLogon'    -Value '1' -Type String
  Set-ItemProperty -Path $winlogon -Name 'DefaultUserName'   -Value $AutoLogonUser -Type String
  Set-ItemProperty -Path $winlogon -Name 'DefaultPassword'   -Value $AutoLogonPassword -Type String
  # Local account -> stamp the machine name so the logon resolves without a
  # domain and glsplay's Resolve-LogonUser can use the bare name.
  Set-ItemProperty -Path $winlogon -Name 'DefaultDomainName' -Value $env:COMPUTERNAME -Type String
  Write-Ok "auto-logon enabled for $AutoLogonUser"
  Write-Warn 'The password is stored in the registry in clear text. This is the'
  Write-Warn 'standard trade-off for headless capture; keep the VM firewalled.'
} elseif ($existing.AutoAdminLogon -eq '1' -and $existing.DefaultUserName) {
  Write-Ok "auto-logon already configured for $($existing.DefaultUserName) - left unchanged"
  Write-Warn 'Pass -AutoLogonPassword (optionally with -AutoLogonUser) to change it.'
} else {
  Write-Warn "Auto-logon not configured. Re-run with -AutoLogonPassword (the user"
  Write-Warn "defaults to the current account, '$AutoLogonUser'), or the console"
  Write-Warn 'session will be empty after every reboot and the host has nothing to capture.'
}

# Never let the console lock or blank - a locked session shows the secure
# desktop, which Desktop Duplication is forbidden from capturing.
Write-Step 'Power and lock settings'
powercfg /setactive SCHEME_MIN 2>$null
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
powercfg /change disk-timeout-ac 0
Write-Ok 'sleep, disk and monitor timeouts disabled'

$policyPath = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization'
New-Item -Path $policyPath -Force | Out-Null
Set-ItemProperty -Path $policyPath -Name 'NoLockScreen' -Value 1 -Type DWord
Write-Ok 'lock screen disabled'

$desktopPath = 'HKCU:\Control Panel\Desktop'
Set-ItemProperty -Path $desktopPath -Name 'ScreenSaveActive' -Value '0' -Type String
Set-ItemProperty -Path $desktopPath -Name 'ScreenSaverIsSecure' -Value '0' -Type String
Write-Ok 'screen saver disabled'

# --- 4. Graphics behaviour --------------------------------------------------

Write-Step 'Graphics'

# Hardware-accelerated GPU scheduling can interfere with Desktop Duplication
# timing on some driver branches. Leaving it on is usually fine, so this only
# reports the state rather than changing it.
$hags = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' `
  -Name 'HwSchMode' -ErrorAction SilentlyContinue
if ($hags) { Write-Ok "HwSchMode = $($hags.HwSchMode)" }

# Remote Desktop compresses and re-composites the desktop. Disabling the
# display driver used by RDP is not possible, but we can at least stop RDP
# from taking over the console session's resolution.
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' `
  -Name 'fSingleSessionPerUser' -Value 1 -Type DWord -ErrorAction SilentlyContinue
Write-Ok 'RDP limited to a single session per user'

# --- 5. Developer prerequisites --------------------------------------------

Write-Step 'Build prerequisites'

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  Write-Warn 'Node.js not found. Needed only if you run the signaling broker here.'
  Write-Warn 'Install: https://nodejs.org/en/download (20 LTS or newer)'
} else {
  Write-Ok "node $(node --version)"
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  Write-Warn 'CMake not found. Needed only if you build the host on this VM.'
  Write-Warn 'Most people cross-build on a workstation and copy the exe across.'
} else {
  Write-Ok "cmake $((cmake --version | Select-Object -First 1))"
}

# --- done -------------------------------------------------------------------

Write-Host ''
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ' Base setup complete' -ForegroundColor White
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host ''
Write-Host ' Next:' -ForegroundColor Cyan
Write-Host '   1. .\install-virtual-display.ps1   (gives the L4 a display head)'
Write-Host '   2. .\setup-firewall.ps1            (opens UDP 50000-50100)'
Write-Host '   3. Reboot'
Write-Host '   4. .\check-environment.ps1         (verifies everything)'
Write-Host ''
Write-Host ' After any RDP session, hand the desktop back to the console:' -ForegroundColor Yellow
Write-Host '   query session'
Write-Host '   tscon <session-id> /dest:console'
Write-Host ''
