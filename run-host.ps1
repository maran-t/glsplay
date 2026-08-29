<#
.SYNOPSIS
  Launches glsplay-host with logging. Started by the logon scheduled task.

.DESCRIPTION
  Exists so the scheduled task can invoke a single file rather than a long
  quoted command line - schtasks mangles nested quotes, and debugging that is
  a worse use of time than keeping a two-line script around.

  Runs in the interactive console session, which Desktop Duplication requires.
  A service (session 0) has no desktop and cannot capture at all.
#>

[CmdletBinding()]
param(
  [string]$Room = 'poc',
  [string]$SignalingUrl = 'ws://localhost:8080',
  [string]$LogPath = 'C:\glsplay\host.log',
  [string]$RepoRoot = 'C:\glsplay',
  # The L4 exposes two DXGI outputs headless: [0] its phantom monitor (~1280x800),
  # [1] the MTT virtual display (1920x1080, pinned by vdd_settings.xml). Capture
  # the MTT one. Set to '' to let the host auto-pick output 0.
  [string]$Output = '1',
  [switch]$NoAudio = $true
)

Set-Location $RepoRoot

# Best-effort: force every attached display to 1920x1080@60. After a tscon the
# console desktop can take several seconds to re-attach its displays, so retry.
& powershell -ExecutionPolicy Bypass -Command {
  param($repo)
  for ($try = 1; $try -le 10; $try++) {
    $out = & powershell -ExecutionPolicy Bypass -File (Join-Path $repo 'vdd\set-vdd-res.ps1') -Width 1920 -Height 1080 -Hz 60 2>&1
    "$(Get-Date -Format o)  attempt $try`n$out"
    if ($out -notmatch 'No display attached') { break }
    Start-Sleep -Seconds 2
  }
} -args $RepoRoot 2>&1 | Out-File "$LogPath.res" -Encoding utf8
Start-Sleep -Seconds 2

$exe = Join-Path $RepoRoot 'apps\host\build\bin\Release\glsplay-host.exe'
if (-not (Test-Path $exe)) {
  "glsplay-host.exe not found at $exe" | Out-File $LogPath -Encoding utf8
  exit 1
}

# Keep the previous run for comparison - the interesting failure is usually
# the one before the restart.
if (Test-Path $LogPath) {
  Move-Item $LogPath "$LogPath.prev" -Force -ErrorAction SilentlyContinue
}

$args = @('--room', $Room, '--signaling-url', $SignalingUrl, '--log-level', 'debug')
if ($NoAudio)          { $args += '--no-audio' }
if ($Output -ne '')    { $args += @('--output', $Output) }

& $exe @args *> $LogPath
