# Moves the interactive user session onto the physical console so the MTT
# virtual display (and therefore the NVIDIA L4's DXGI output) becomes active
# for Desktop Duplication. Meant to run as SYSTEM from a RemoteDisconnect task.

$log = 'C:\glsplay\reclaim-console.log'
"$(Get-Date -Format o)  reclaim-console starting" | Out-File $log -Append -Encoding utf8

$sid = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" |
       Select-Object -ExpandProperty SessionId -Unique | Select-Object -First 1

if ($sid) {
  $out = & "$env:SystemRoot\System32\tscon.exe" $sid /dest:console 2>&1
  "$(Get-Date -Format o)  tscon $sid /dest:console -> exit $LASTEXITCODE : $out" | Out-File $log -Append -Encoding utf8
  Start-Sleep -Seconds 4

  # The session move can leave the VDD at a wrong mode (see DEPLOY-GCP-L4 9.9).
  # Re-pin it here so a still-running host re-acquires DXGI at the right size.
  & powershell -ExecutionPolicy Bypass -File 'C:\glsplay\vdd\set-vdd-res.ps1' -Width 1920 -Height 1080 -Hz 60 2>&1 |
    Out-File $log -Append -Encoding utf8

  # Only (re)start the host if it actually died. With the ClosePeer() crash
  # fixed it survives client churn and the RDP in/out cycle on its own (the
  # capture loop retries Reinitialise() on ACCESS_LOST), so this is now just a
  # safety net for a genuinely-down host - not the normal path.
  if (Get-Process glsplay-host -ErrorAction SilentlyContinue) {
    "$(Get-Date -Format o)  glsplay-host already running - not restarting" | Out-File $log -Append -Encoding utf8
  } else {
    $r = & "$env:SystemRoot\System32\schtasks.exe" /run /tn glsplay-host 2>&1
    "$(Get-Date -Format o)  glsplay-host was down - schtasks /run glsplay-host -> $r" | Out-File $log -Append -Encoding utf8
  }
} else {
  "$(Get-Date -Format o)  no explorer.exe session found - is the autologon user logged in?" | Out-File $log -Append -Encoding utf8
}
