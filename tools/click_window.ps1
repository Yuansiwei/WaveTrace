param(
    [Parameter(Mandatory = $true)]
    [long]$WindowHandle,
    [Parameter(Mandatory = $true)]
    [int]$X,
    [Parameter(Mandatory = $true)]
    [int]$Y,
    [ValidateSet("left", "right", "middle", "l", "r", "m")]
    [string]$MouseButton = "left",
    [int]$ClickCount = 1,
    [switch]$BringToFront
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$source = @"
using System;
using System.Runtime.InteropServices;

public static class WinClickNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attribute, out RECT rect, int size);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);

    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    public const int SW_RESTORE = 9;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    public const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
    public const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
}
"@

if (-not ("WinClickNative" -as [type])) {
    Add-Type -TypeDefinition $source
}

[void][WinClickNative]::SetProcessDPIAware()

function Get-CaptureRectangle([IntPtr]$Handle) {
    $rect = New-Object WinClickNative+RECT
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type]"WinClickNative+RECT")
    $hr = [WinClickNative]::DwmGetWindowAttribute(
        $Handle,
        [WinClickNative]::DWMWA_EXTENDED_FRAME_BOUNDS,
        [ref]$rect,
        $size)

    if ($hr -ne 0) {
        if (-not [WinClickNative]::GetWindowRect($Handle, [ref]$rect)) {
            throw "GetWindowRect failed."
        }
    }

    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Window has invalid bounds: ${width}x${height}."
    }

    return New-Object System.Drawing.Rectangle($rect.Left, $rect.Top, $width, $height)
}

$handle = [IntPtr]$WindowHandle
if (-not [WinClickNative]::IsWindow($handle)) {
    throw "Window handle 0x$($WindowHandle.ToString('x')) is not a valid window."
}

if ($ClickCount -lt 1) {
    throw "ClickCount must be >= 1."
}

if ([WinClickNative]::IsIconic($handle)) {
    [void][WinClickNative]::ShowWindow($handle, [WinClickNative]::SW_RESTORE)
    Start-Sleep -Milliseconds 150
}

if ($BringToFront) {
    [void][WinClickNative]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 120
}

$rect = Get-CaptureRectangle $handle
$screenX = $rect.X + $X
$screenY = $rect.Y + $Y
if (-not [WinClickNative]::SetCursorPos($screenX, $screenY)) {
    throw "SetCursorPos failed."
}

switch ($MouseButton) {
    "right" { $down = [WinClickNative]::MOUSEEVENTF_RIGHTDOWN; $up = [WinClickNative]::MOUSEEVENTF_RIGHTUP }
    "r" { $down = [WinClickNative]::MOUSEEVENTF_RIGHTDOWN; $up = [WinClickNative]::MOUSEEVENTF_RIGHTUP }
    "middle" { $down = [WinClickNative]::MOUSEEVENTF_MIDDLEDOWN; $up = [WinClickNative]::MOUSEEVENTF_MIDDLEUP }
    "m" { $down = [WinClickNative]::MOUSEEVENTF_MIDDLEDOWN; $up = [WinClickNative]::MOUSEEVENTF_MIDDLEUP }
    default { $down = [WinClickNative]::MOUSEEVENTF_LEFTDOWN; $up = [WinClickNative]::MOUSEEVENTF_LEFTUP }
}

for ($i = 0; $i -lt $ClickCount; $i += 1) {
    [WinClickNative]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 30
    [WinClickNative]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
    if ($i + 1 -lt $ClickCount) {
        Start-Sleep -Milliseconds 90
    }
}

[pscustomobject]@{
    WindowHandle = $WindowHandle
    WindowX = $X
    WindowY = $Y
    ScreenX = $screenX
    ScreenY = $screenY
    MouseButton = $MouseButton
    ClickCount = $ClickCount
} | ConvertTo-Json -Compress
