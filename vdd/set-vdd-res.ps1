param([int]$Width = 1920, [int]$Height = 1080, [int]$Hz = 60)

# Makes the MTT virtual display the SOLE, PRIMARY desktop at a fixed mode:
#  - MTT  -> 1920x1080@60, position (0,0), primary
#  - every other attached display -> detached
# so the real desktop (taskbar, windows) renders on the display glsplay-host
# captures, instead of a blank extended monitor.
#
# No external modules. Must run in the session that owns the displays (the
# console/autologon session with RDP disconnected).

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VddRes {
  [DllImport("user32.dll")] public static extern bool EnumDisplayDevices(string dev, uint num, ref DISPLAY_DEVICE d, uint flags);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int ChangeDisplaySettingsEx(string dev, ref DEVMODE dm, IntPtr hwnd, uint flags, IntPtr lp);
  [DllImport("user32.dll")] public static extern int ChangeDisplaySettingsEx(string dev, IntPtr dm, IntPtr hwnd, uint flags, IntPtr lp);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)]  public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
    public int StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public int dmFields;
    public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
    public short dmLogPixels;
    public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
  }
}
"@

$DM_POSITION          = 0x00000020
$DM_PELSWIDTH         = 0x00080000
$DM_PELSHEIGHT        = 0x00100000
$DM_DISPLAYFREQUENCY  = 0x00400000
$CDS_UPDATEREGISTRY   = 0x00000001
$CDS_SET_PRIMARY      = 0x00000010
$CDS_NORESET          = 0x10000000
$ATTACHED_TO_DESKTOP  = 0x1

# --- enumerate attached displays -----------------------------------------
$displays = @()
for ($i = 0; ; $i++) {
  $dd = New-Object VddRes+DISPLAY_DEVICE
  $dd.cb = [Runtime.InteropServices.Marshal]::SizeOf($dd)
  if (-not [VddRes]::EnumDisplayDevices($null, [uint32]$i, [ref]$dd, 0)) { break }
  if (($dd.StateFlags -band $ATTACHED_TO_DESKTOP) -eq 0) { continue }
  $displays += ,$dd
}
if ($displays.Count -eq 0) { Write-Warning "No display attached to the desktop in this session."; exit 1 }

$mtt = $displays | Where-Object { $_.DeviceString -match 'MTT|Virtual Display Driver' } | Select-Object -First 1
if (-not $mtt) {
  Write-Warning "MTT virtual display not attached yet (have: $(( $displays | ForEach-Object { $_.DeviceString }) -join ', '))."
  exit 2
}

function New-Devmode {
  $dm = New-Object VddRes+DEVMODE
  $dm.dmSize = [int16][Runtime.InteropServices.Marshal]::SizeOf([type]([VddRes+DEVMODE]))
  return $dm
}

# --- MTT: primary, at target mode, at origin (batched) ------------------
$dm = New-Devmode
[void][VddRes]::EnumDisplaySettings($mtt.DeviceName, -1, [ref]$dm)
$dm.dmPelsWidth = $Width; $dm.dmPelsHeight = $Height; $dm.dmDisplayFrequency = $Hz
$dm.dmPositionX = 0; $dm.dmPositionY = 0
$dm.dmFields = $DM_POSITION -bor $DM_PELSWIDTH -bor $DM_PELSHEIGHT -bor $DM_DISPLAYFREQUENCY
$r = [VddRes]::ChangeDisplaySettingsEx($mtt.DeviceName, [ref]$dm, [IntPtr]::Zero,
      ($CDS_UPDATEREGISTRY -bor $CDS_SET_PRIMARY -bor $CDS_NORESET), [IntPtr]::Zero)
Write-Host "MTT  $($mtt.DeviceName)  -> ${Width}x${Height}@${Hz} primary  queued=$r"

# --- every other display: detach (batched) ----------------------------
foreach ($d in $displays) {
  if ($d.DeviceName -eq $mtt.DeviceName) { continue }
  $z = New-Devmode
  [void][VddRes]::EnumDisplaySettings($d.DeviceName, -1, [ref]$z)
  $z.dmPelsWidth = 0; $z.dmPelsHeight = 0; $z.dmPositionX = 0; $z.dmPositionY = 0
  $z.dmFields = $DM_POSITION -bor $DM_PELSWIDTH -bor $DM_PELSHEIGHT
  $r = [VddRes]::ChangeDisplaySettingsEx($d.DeviceName, [ref]$z, [IntPtr]::Zero,
        ($CDS_UPDATEREGISTRY -bor $CDS_NORESET), [IntPtr]::Zero)
  Write-Host "detach  $($d.DeviceName)  ($($d.DeviceString))  queued=$r"
}

# --- apply the batch --------------------------------------------------
$apply = [VddRes]::ChangeDisplaySettingsEx($null, [IntPtr]::Zero, [IntPtr]::Zero, 0, [IntPtr]::Zero)
Write-Host "apply -> $apply   (0 = DISP_CHANGE_SUCCESSFUL)"
exit ([math]::Max(0, $apply))
