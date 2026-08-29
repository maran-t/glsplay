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
  # The tscon fires a ConsoleConnect for this session; glsplay-display and
  # glsplay-host are bound to that trigger and run AS the user in the console
  # session (the only context where display config + Desktop Duplication work).
  # Nothing else to do here.
} else {
  "$(Get-Date -Format o)  no explorer.exe session found - is the autologon user logged in?" | Out-File $log -Append -Encoding utf8
}
