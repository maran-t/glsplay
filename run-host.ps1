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
  [switch]$NoAudio = $true
)

Set-Location $RepoRoot

# Make the MTT virtual display the sole, primary desktop at 1920x1080@60. After
# a tscon the console can take a few seconds to re-attach its displays, so retry.
# Exit 0  = MTT is now the only output  -> let the host auto-pick output 0.
# Exit >0 = MTT not ready / still extended -> pin the host to output 1 (MTT).
"$(Get-Date -Format o)  set-vdd-res run" | Out-File "$LogPath.res" -Encoding utf8
$resOk = $false
for ($try = 1; $try -le 12; $try++) {
  $out = & powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'vdd\set-vdd-res.ps1') -Width 1920 -Height 1080 -Hz 60 2>&1
  "$(Get-Date -Format o)  attempt $try (exit $LASTEXITCODE)`n$out`n" | Out-File "$LogPath.res" -Append -Encoding utf8
  if ($LASTEXITCODE -eq 0) { $resOk = $true; break }
  Start-Sleep -Seconds 2
}
$Output = if ($resOk) { '' } else { '1' }
"$(Get-Date -Format o)  resOk=$resOk  -> --output '$Output'" | Out-File "$LogPath.res" -Append -Encoding utf8
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
