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
  [string]$SignalingUrl = 'wss://play.resrve.xyz/ws',
  [string]$LogPath = 'C:\glsplay\host.log',
  [string]$RepoRoot = 'C:\glsplay',
  # DXGI output to duplicate on the NVIDIA L4 adapter:
  #   0 = the L4's own phantom head (~1280x800)
  #   1 = the Virtual Display Driver monitor (1920x1080) <-- the one we want
  # host.log logs "DXGI adapters: [0] NVIDIA L4 outputs=2" - if that count or
  # ordering ever changes, revisit this.
  [string]$Output = '1',
  [switch]$NoAudio = $true
)

Set-Location $RepoRoot

# Pin the MTT virtual display to a fixed mode before capture starts. A wrong or
# changing resolution makes NVENC reject the live ReconfigureEncoder call.
& powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'vdd\set-vdd-res.ps1') -Width 1920 -Height 1080 -Hz 60 2>&1 |
  Out-File "$LogPath.res" -Encoding utf8
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
if ($NoAudio)       { $args += '--no-audio' }
if ($Output -ne '') { $args += @('--output', $Output) }

& $exe @args *> $LogPath
