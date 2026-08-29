<#
.SYNOPSIS
  Installs an Indirect Display Driver so the headless L4 has a display to capture.

.DESCRIPTION
  A data-center L4 has no display head. Without a virtual monitor, DXGI
  Desktop Duplication has nothing to duplicate and DuplicateOutput fails with
  DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (PRD section 4.1).

  IddCx is Microsoft's driver model for this, shipped in the WDK. Microsoft
  publishes a sample rather than a distributable binary, so you need a concrete
  driver built on it. This script does not silently download an unsigned
  kernel-adjacent driver on your behalf - it checks what is present and tells
  you exactly what to install.

  Options, all IddCx-based:
    1. Parsec VDD          - signed by Parsec, free, install and go
    2. Microsoft's sample  - Windows-driver-samples/video/IndirectDisplay,
                             build with the WDK and sign it yourself
    3. IddSampleDriver     - community build of that sample, self-signed

  IddCx drivers are user-mode (UMDF), so unlike a kernel driver this does not
  normally require disabling Secure Boot.
#>

[CmdletBinding()]
param(
  # Path to an already-downloaded driver package containing a .inf file.
  [string]$DriverPath,

  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$RefreshHz = 60
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
      ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script as Administrator.'
}

function Write-Step { param([string]$m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "    [!!] $m" -ForegroundColor Yellow }

# --- what is already there --------------------------------------------------

Write-Step 'Checking for an existing virtual display'

$existing = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
            Where-Object { $_.FriendlyName -match 'Idd|Indirect|Virtual|Parsec' }

if ($existing) {
  foreach ($d in $existing) { Write-Ok "$($d.FriendlyName)  status=$($d.Status)" }
  Write-Host ''
  Write-Host 'A virtual display driver is already installed.' -ForegroundColor Green
} else {
  Write-Warn 'No indirect display driver found'
}

Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
$screens = [System.Windows.Forms.Screen]::AllScreens
Write-Step "Displays currently attached: $($screens.Count)"
foreach ($s in $screens) {
  Write-Host "    $($s.DeviceName)  $($s.Bounds.Width)x$($s.Bounds.Height)  primary=$($s.Primary)"
}

# --- install from a supplied package ---------------------------------------

if ($DriverPath) {
  Write-Step "Installing driver from $DriverPath"

  if (-not (Test-Path $DriverPath)) { throw "Path not found: $DriverPath" }

  $inf = Get-ChildItem -Path $DriverPath -Filter *.inf -Recurse | Select-Object -First 1
  if (-not $inf) { throw "No .inf file found under $DriverPath" }
  Write-Ok "found $($inf.Name)"

  # A self-signed or test-signed package needs its certificate trusted first,
  # otherwise pnputil rejects it without explaining why.
  $cert = Get-ChildItem -Path $DriverPath -Filter *.cer -Recurse | Select-Object -First 1
  if ($cert) {
    Write-Ok "importing certificate $($cert.Name)"
    certutil -addstore -f "TrustedPublisher" $cert.FullName | Out-Null
    certutil -addstore -f "Root" $cert.FullName | Out-Null
  }

  Write-Host '    running pnputil...' -ForegroundColor DarkGray
  $result = pnputil /add-driver $inf.FullName /install 2>&1
  $result | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

  if ($LASTEXITCODE -ne 0) {
    throw "pnputil failed with exit code $LASTEXITCODE"
  }
  Write-Ok 'driver installed'

  Write-Host ''
  Write-Host 'Reboot, then re-run this script to confirm the display appeared.' -ForegroundColor Cyan
  return
}

# --- guidance ---------------------------------------------------------------

if (-not $existing) {
  Write-Host ''
  Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
  Write-Host ' No virtual display driver installed' -ForegroundColor Yellow
  Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
  Write-Host ''
  Write-Host ' Pick one, download it, then re-run with -DriverPath:' -ForegroundColor Cyan
  Write-Host ''
  Write-Host '  1. Parsec Virtual Display Driver  (simplest - already signed)'
  Write-Host '     https://github.com/parsec-cloud/Parsec-vdd'
  Write-Host ''
  Write-Host '  2. Microsoft IndirectDisplay sample  (most official lineage)'
  Write-Host '     https://github.com/microsoft/Windows-driver-samples'
  Write-Host '       -> video/IndirectDisplay'
  Write-Host '     Build with the WDK, add your mode to the driver mode list, sign it.'
  Write-Host ''
  Write-Host '  3. IddSampleDriver  (community build of the sample, self-signed)'
  Write-Host '     Ships an options file for configuring resolutions.'
  Write-Host ''
  Write-Host ' Then:' -ForegroundColor Cyan
  Write-Host "   .\install-virtual-display.ps1 -DriverPath C:\path\to\driver"
  Write-Host ''
  Write-Host " Whichever you choose, its mode list must include ${Width}x${Height} @ ${RefreshHz}Hz," -ForegroundColor Yellow
  Write-Host ' or Windows will never offer that resolution (PRD section 4.1).' -ForegroundColor Yellow
  Write-Host ''
  Write-Host ' Bind it to the NVIDIA adapter, not the Basic Display Adapter - otherwise' -ForegroundColor Yellow
  Write-Host ' Desktop Duplication returns textures on the wrong device and the' -ForegroundColor Yellow
  Write-Host ' zero-copy path to NVENC silently degrades to a CPU roundtrip.' -ForegroundColor Yellow
  Write-Host ''
  exit 1
}

# --- verify the mode --------------------------------------------------------

Write-Step 'Verifying the target mode'

$primary = [System.Windows.Forms.Screen]::PrimaryScreen
if ($primary.Bounds.Width -eq $Width -and $primary.Bounds.Height -eq $Height) {
  Write-Ok "primary display is ${Width}x${Height}"
} else {
  Write-Warn "primary display is $($primary.Bounds.Width)x$($primary.Bounds.Height), expected ${Width}x${Height}"
  Write-Warn 'Select the correct mode in Display Settings, or add it to the driver mode list.'
}

$refresh = (Get-CimInstance Win32_VideoController |
            Where-Object { $_.CurrentRefreshRate } | Select-Object -First 1).CurrentRefreshRate
if ($refresh -ge $RefreshHz) {
  Write-Ok "refresh rate ${refresh}Hz"
} else {
  Write-Warn "refresh rate is ${refresh}Hz, expected at least ${RefreshHz}Hz - capture cannot exceed it"
}

Write-Host ''
Write-Host 'Virtual display looks usable. Run check-environment.ps1 for the full picture.' -ForegroundColor Green
Write-Host ''
