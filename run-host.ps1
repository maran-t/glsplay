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
  #   0 = the 1920x1080 desktop <-- the one we want
  #   1 = the L4's own phantom head (~1280x800)
  #
  # This ordering is not stable. DXGI re-enumerates the adapter's outputs when
  # the display layout changes, and the primary display generally lands on
  # output 0 - so marking the 1080p monitor primary (which is what makes a game
  # take exclusive fullscreen on the captured display) swapped 0 and 1 from what
  # they used to be. The symptom is quiet: the stream just comes up at the wrong
  # resolution. Check "DXGI duplication ready: NVIDIA L4 <w>x<h>" in host.log
  # after any display change, and flip this if it reads the wrong size.
  [string]$Output = '0',
  # Default to audio ON. Pass -NoAudio to disable (e.g. on a VM with no
  # virtual audio endpoint installed).
  [switch]$NoAudio = $false
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
