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
  # DXGI output to duplicate. Leave '' (auto = output 0): set-vdd-res makes the
  # MTT display the sole/primary output, so output 0 carries the real desktop.
  # If set-vdd-res fails, output 0 is the L4 phantom (1280x800) - still the
  # desktop, just low-res. Never blank. Only set this if you know better.
  [string]$Output = '',
  [switch]$NoAudio = $true
)

Set-Location $RepoRoot

# The glsplay-display task (ConsoleConnect trigger) does the real display config
# in the console session. Run it again here as a fallback - the window-station
# fix in set-vdd-res.ps1 may let it work from this context too.
"$(Get-Date -Format o)  run-host fallback set-vdd-res" | Out-File "$LogPath.res" -Encoding utf8
for ($try = 1; $try -le 8; $try++) {
  $o = & powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'vdd\set-vdd-res.ps1') -Width 1920 -Height 1080 -Hz 60 2>&1
  "$(Get-Date -Format o)  attempt $try (exit $LASTEXITCODE)`n$o`n" | Out-File "$LogPath.res" -Append -Encoding utf8
  if ($LASTEXITCODE -eq 0) { break }
  Start-Sleep -Seconds 2
}
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
