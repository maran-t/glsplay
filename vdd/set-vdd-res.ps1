param([int]$Width = 1920, [int]$Height = 1080, [int]$Hz = 60)

# Sets the MTT virtual display to a fixed mode via Win32 ChangeDisplaySettingsEx.
# No external modules. Must run in the session that owns the display (i.e. the
# console/autologon session with RDP disconnected).

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VddRes {
  [DllImport("user32.dll")] public static extern bool EnumDisplayDevices(string dev, uint num, ref DISPLAY_DEVICE d, uint flags);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int ChangeDisplaySettingsEx(string dev, ref DEVMODE dm, IntPtr hwnd, uint flags, IntPtr lp);
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

$DM_PELSWIDTH        = 0x80000
$DM_PELSHEIGHT       = 0x100000
$DM_DISPLAYFREQUENCY = 0x400000
$CDS_UPDATEREGISTRY  = 0x01
$found = $false

for ($i = 0; ; $i++) {
  $dd = New-Object VddRes+DISPLAY_DEVICE
  $dd.cb = [Runtime.InteropServices.Marshal]::SizeOf($dd)
  if (-not [VddRes]::EnumDisplayDevices($null, [uint32]$i, [ref]$dd, 0)) { break }
  if ($dd.DeviceString -match 'MTT|Virtual Display') {
    $found = $true
    Write-Host "target: $($dd.DeviceName)  ($($dd.DeviceString))  flags=$($dd.StateFlags)"
    $dm = New-Object VddRes+DEVMODE
    $dm.dmSize = [int16][Runtime.InteropServices.Marshal]::SizeOf([type]([VddRes+DEVMODE]))
    [void][VddRes]::EnumDisplaySettings($dd.DeviceName, -1, [ref]$dm)
    $dm.dmPelsWidth        = $Width
    $dm.dmPelsHeight       = $Height
    $dm.dmDisplayFrequency = $Hz
    $dm.dmFields = $DM_PELSWIDTH -bor $DM_PELSHEIGHT -bor $DM_DISPLAYFREQUENCY
    $r = [VddRes]::ChangeDisplaySettingsEx($dd.DeviceName, [ref]$dm, [IntPtr]::Zero, $CDS_UPDATEREGISTRY, [IntPtr]::Zero)
    Write-Host "ChangeDisplaySettingsEx($Width x $Height @ $Hz) -> $r   (0 = DISP_CHANGE_SUCCESSFUL)"
  }
}
if (-not $found) { Write-Warning "No MTT / Virtual Display device found in EnumDisplayDevices." }
