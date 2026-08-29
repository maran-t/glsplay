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

  # The tscon fires a ConsoleConnect for this session; glsplay-display (a task
  # bound to that trigger, running AS the user) does the display config in the
  # console session's own context - the only place EnumDisplayDevices works.
  # Give it, and the display topology, time to settle before capture starts.
  Start-Sleep -Seconds 12

  $r = & "$env:SystemRoot\System32\schtasks.exe" /run /tn glsplay-host 2>&1
  "$(Get-Date -Format o)  schtasks /run glsplay-host -> $r" | Out-File $log -Append -Encoding utf8
} else {
  "$(Get-Date -Format o)  no explorer.exe session found - is the autologon user logged in?" | Out-File $log -Append -Encoding utf8
}
