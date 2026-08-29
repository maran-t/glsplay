<#
.SYNOPSIS
  Fetches the host's three build dependencies into apps/host/third_party.

.DESCRIPTION
  Two of the three can be downloaded automatically. The NVIDIA Video Codec SDK
  cannot - it sits behind a licence acceptance and a developer login, so this
  script tells you exactly what to place where instead of pretending otherwise.

  Run from anywhere; paths are resolved relative to the repo.

.EXAMPLE
  .\fetch-deps.ps1
  .\fetch-deps.ps1 -WebrtcVersion m152.7977.0.0 -SkipViGEm
#>

[CmdletBinding()]
param(
  # Shiguredo webrtc-build release tag. This repo is built and tested against
  # m152.7977.0.0 - the libwebrtc API drifts between releases (see
  # docs/BUILD-HOST.md), so use this exact tag unless you are prepared to fix
  # signature mismatches. Override to try a newer one.
  [string]$WebrtcVersion = 'm152.7977.0.0',
  [switch]$SkipWebrtc,
  [switch]$SkipViGEm
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$thirdParty = Join-Path $repoRoot 'apps\host\third_party'
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

function Write-Step { param([string]$m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "    [!!] $m" -ForegroundColor Yellow }

Write-Host "third_party: $thirdParty" -ForegroundColor DarkGray

# --- 1. libwebrtc (Shiguredo prebuilt) --------------------------------------

Write-Step 'Google libwebrtc (Shiguredo webrtc-build prebuilt)'

$webrtcDir = Join-Path $thirdParty 'webrtc'
if (Test-Path (Join-Path $webrtcDir 'include')) {
  Write-Ok 'already present'
} elseif ($SkipWebrtc) {
  Write-Warn 'skipped'
} else {
  if (-not $WebrtcVersion) {
    Write-Warn 'No -WebrtcVersion given. Pick a release tag from:'
    Write-Host '      https://github.com/shiguredo-webrtc-build/webrtc-build/releases' -ForegroundColor DarkGray
    Write-Host '    Download the windows_x86_64 archive, extract it to:' -ForegroundColor DarkGray
    Write-Host "      $webrtcDir" -ForegroundColor DarkGray
    Write-Host '    so that include\ and lib\ sit directly inside it.' -ForegroundColor DarkGray
  } else {
    $url = "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/$WebrtcVersion/webrtc.windows_x86_64.zip"
    $archive = Join-Path $env:TEMP "webrtc-$WebrtcVersion.zip"
    Write-Host "    downloading $url" -ForegroundColor DarkGray
    try {
      Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
      $staging = Join-Path $env:TEMP "webrtc-extract-$WebrtcVersion"
      if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
      Expand-Archive -Path $archive -DestinationPath $staging -Force

      # Releases have varied in whether they nest everything under a top-level
      # folder, so locate include\ and take whatever directory contains it
      # rather than assuming a fixed depth.
      $found = Get-ChildItem $staging -Recurse -Directory -Filter 'include' |
               Select-Object -First 1
      if (-not $found) { throw "no include\ directory inside the archive" }

      if (Test-Path $webrtcDir) { Remove-Item $webrtcDir -Recurse -Force }
      Move-Item $found.Parent.FullName $webrtcDir -Force
      Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
      Write-Ok "extracted to $webrtcDir"
    } catch {
      Write-Warn "download failed: $($_.Exception.Message)"
      Write-Warn 'Download manually from the releases page and extract to the path above.'
    }
  }
}

# --- 2. NVIDIA Video Codec SDK ----------------------------------------------

Write-Step 'NVIDIA Video Codec SDK (NVENC headers)'

$nvencDir = Join-Path $thirdParty 'nvenc'
if (Test-Path (Join-Path $nvencDir 'Interface\nvEncodeAPI.h')) {
  Write-Ok 'already present'
} else {
  Write-Warn 'Cannot be downloaded automatically - it requires accepting the'
  Write-Warn 'NVIDIA SDK licence with a developer account.'
  Write-Host ''
  Write-Host '    1. https://developer.nvidia.com/nvidia-video-codec-sdk/download' -ForegroundColor DarkGray
  Write-Host '    2. Extract the archive' -ForegroundColor DarkGray
  Write-Host "    3. Copy its Interface\ folder to: $nvencDir\Interface\" -ForegroundColor DarkGray
  Write-Host ''
  Write-Host '    Only the headers are needed. nvEncodeAPI64.dll ships with the' -ForegroundColor DarkGray
  Write-Host '    driver and is loaded at runtime, so there is nothing to link.' -ForegroundColor DarkGray
}

# --- 3. ViGEmClient ---------------------------------------------------------

Write-Step 'ViGEmClient (gamepad, PRD phase 5 only)'

$vigemDir = Join-Path $thirdParty 'ViGEmClient'
if (Test-Path (Join-Path $vigemDir 'include\ViGEm\Client.h')) {
  Write-Ok 'already present'
} elseif ($SkipViGEm) {
  Write-Warn 'skipped - host will build with NullGamepadSink'
} else {
  Write-Warn 'Not present. This is optional - the host builds and runs without it.'
  Write-Host ''
  Write-Host '    Source: https://github.com/nefarius/ViGEmClient' -ForegroundColor DarkGray
  Write-Host '    Build it, then place include\ and lib\ under:' -ForegroundColor DarkGray
  Write-Host "      $vigemDir" -ForegroundColor DarkGray
  Write-Host ''
  Write-Host '    The ViGEmBus driver is separate and installs on the VM:' -ForegroundColor DarkGray
  Write-Host '      https://github.com/nefarius/ViGEmBus/releases' -ForegroundColor DarkGray
  Write-Host ''
  Write-Host '    Or build without it: cmake .. -DGLSPLAY_ENABLE_VIGEM=OFF' -ForegroundColor DarkGray
}

# --- summary ----------------------------------------------------------------

Write-Host ''
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
$haveWebrtc = Test-Path (Join-Path $webrtcDir 'include')
$haveNvenc  = Test-Path (Join-Path $nvencDir 'Interface\nvEncodeAPI.h')
$haveVigem  = Test-Path (Join-Path $vigemDir 'include\ViGEm\Client.h')

Write-Host ("  libwebrtc  : " + $(if ($haveWebrtc) { 'ready' } else { 'MISSING (required)' })) `
  -ForegroundColor $(if ($haveWebrtc) { 'Green' } else { 'Red' })
Write-Host ("  NVENC SDK  : " + $(if ($haveNvenc) { 'ready' } else { 'MISSING (required)' })) `
  -ForegroundColor $(if ($haveNvenc) { 'Green' } else { 'Red' })
Write-Host ("  ViGEmClient: " + $(if ($haveVigem) { 'ready' } else { 'absent (optional)' })) `
  -ForegroundColor $(if ($haveVigem) { 'Green' } else { 'Yellow' })
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray

if ($haveWebrtc -and $haveNvenc) {
  Write-Host ''
  Write-Host ' Ready to build:' -ForegroundColor Cyan
  Write-Host '   cd apps\host'
  Write-Host '   cmake -B build -G "Visual Studio 17 2022" -A x64'
  Write-Host '   cmake --build build --config Release'
  Write-Host ''
} else {
  Write-Host ''
  Write-Host ' Install the missing dependencies above, then re-run this script.' -ForegroundColor Yellow
  Write-Host ''
  exit 1
}
