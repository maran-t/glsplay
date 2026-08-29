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
  # Which DXGI output to duplicate. The MTT virtual display is output 1 on the
  # L4 (output 0 is the L4's phantom monitor). The glsplay-display task makes
  # MTT the primary/sole display; capturing output 1 gets the real desktop at
  # 1920x1080. Set to '' only if you know MTT is the sole output.
  [string]$Output = '1',
  [switch]$NoAudio = $true
)

Set-Location $RepoRoot

# Display config (MTT primary + others detached) is done by the glsplay-display
# task on the ConsoleConnect that reclaim-console.ps1's tscon produced - that
# task runs in the console session where EnumDisplayDevices actually works. Here
# we just give it a moment to have finished.
Start-Sleep -Seconds 3

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
