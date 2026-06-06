param(
    [long]$WindowHandle = 0,
    [string]$ProcessName,
    [string]$TitleRegex,
    [Parameter(Mandatory = $true)]
    [string]$OutPath,
    [ValidateSet("Auto", "PrintWindow", "Screen")]
    [string]$Method = "Auto",
    [switch]$BringToFront
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$source = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WinCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextLength(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attribute, out RECT rect, int size);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, int flags);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);

    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    public const int PW_RENDERFULLCONTENT = 2;
    public const int SW_RESTORE = 9;
}
"@

if (-not ("WinCaptureNative" -as [type])) {
    Add-Type -TypeDefinition $source
}

function Get-WindowTitle([IntPtr]$Handle) {
    $length = [WinCaptureNative]::GetWindowTextLength($Handle)
    if ($length -le 0) {
        return ""
    }
    $builder = New-Object System.Text.StringBuilder($length + 1)
    [void][WinCaptureNative]::GetWindowText($Handle, $builder, $builder.Capacity)
    return $builder.ToString()
}

function Get-TargetProcess {
    if ($WindowHandle -ne 0) {
        $handle = [IntPtr]$WindowHandle
        if (-not [WinCaptureNative]::IsWindow($handle)) {
            throw "Window handle 0x$($WindowHandle.ToString('x')) is not a valid window."
        }

        [uint32]$targetPid = 0
        [void][WinCaptureNative]::GetWindowThreadProcessId($handle, [ref]$targetPid)
        $process = Get-Process -Id ([int]$targetPid) -ErrorAction SilentlyContinue
        $processName = if ($process) { $process.ProcessName } else { "" }
        return [pscustomobject]@{
            Id = [int]$targetPid
            ProcessName = $processName
            MainWindowTitle = Get-WindowTitle $handle
            MainWindowHandle = $handle
        }
    }

    $candidates = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 }

    if ($ProcessName) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($ProcessName)
        $candidates = $candidates | Where-Object { $_.ProcessName -ieq $name }
    }

    if ($TitleRegex) {
        $candidates = $candidates | Where-Object { $_.MainWindowTitle -match $TitleRegex }
    }

    $match = $candidates | Sort-Object StartTime -Descending | Select-Object -First 1
    if (-not $match) {
        throw "No matching top-level window found."
    }
    return $match
}

function Get-CaptureRectangle([IntPtr]$Handle) {
    $rect = New-Object WinCaptureNative+RECT
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type]"WinCaptureNative+RECT")
    $hr = [WinCaptureNative]::DwmGetWindowAttribute(
        $Handle,
        [WinCaptureNative]::DWMWA_EXTENDED_FRAME_BOUNDS,
        [ref]$rect,
        $size)

    if ($hr -ne 0) {
        if (-not [WinCaptureNative]::GetWindowRect($Handle, [ref]$rect)) {
            throw "GetWindowRect failed."
        }
    }

    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Window has invalid capture bounds: ${width}x${height}."
    }

    return New-Object System.Drawing.Rectangle($rect.Left, $rect.Top, $width, $height)
}

function Test-BitmapHasContent([System.Drawing.Bitmap]$Bitmap) {
    $width = $Bitmap.Width
    $height = $Bitmap.Height
    if ($width -le 0 -or $height -le 0) {
        return $false
    }

    $colors = New-Object 'System.Collections.Generic.HashSet[int]'
    $stepX = [Math]::Max(1, [Math]::Floor($width / 24))
    $stepY = [Math]::Max(1, [Math]::Floor($height / 24))

    for ($y = 0; $y -lt $height; $y += $stepY) {
        for ($x = 0; $x -lt $width; $x += $stepX) {
            [void]$colors.Add($Bitmap.GetPixel($x, $y).ToArgb())
            if ($colors.Count -ge 6) {
                return $true
            }
        }
    }

    return $colors.Count -ge 3
}

function Save-Bitmap([System.Drawing.Bitmap]$Bitmap, [string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $dir = [System.IO.Path]::GetDirectoryName($fullPath)
    if ($dir -and -not [System.IO.Directory]::Exists($dir)) {
        [System.IO.Directory]::CreateDirectory($dir) | Out-Null
    }
    $Bitmap.Save($fullPath, [System.Drawing.Imaging.ImageFormat]::Png)
    return $fullPath
}

function Capture-PrintWindow([IntPtr]$Handle, [System.Drawing.Rectangle]$Rect) {
    $bitmap = New-Object System.Drawing.Bitmap($Rect.Width, $Rect.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Magenta)
        $hdc = $graphics.GetHdc()
        try {
            $ok = [WinCaptureNative]::PrintWindow($Handle, $hdc, [WinCaptureNative]::PW_RENDERFULLCONTENT)
        } finally {
            $graphics.ReleaseHdc($hdc)
        }

        if (-not $ok) {
            $bitmap.Dispose()
            return $null
        }

        return $bitmap
    } finally {
        $graphics.Dispose()
    }
}

function Capture-Screen([IntPtr]$Handle, [System.Drawing.Rectangle]$Rect, [bool]$FocusWindow) {
    if ([WinCaptureNative]::IsIconic($Handle)) {
        [void][WinCaptureNative]::ShowWindow($Handle, [WinCaptureNative]::SW_RESTORE)
        Start-Sleep -Milliseconds 200
    }

    if ($FocusWindow) {
        [void][WinCaptureNative]::SetForegroundWindow($Handle)
        Start-Sleep -Milliseconds 250
    }

    $bitmap = New-Object System.Drawing.Bitmap($Rect.Width, $Rect.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($Rect.Location, [System.Drawing.Point]::Empty, $Rect.Size)
        return $bitmap
    } finally {
        $graphics.Dispose()
    }
}

$process = Get-TargetProcess
$handle = [IntPtr]$process.MainWindowHandle
$rect = Get-CaptureRectangle $handle
$usedMethod = $null
$bitmap = $null

if ($Method -eq "Auto" -or $Method -eq "PrintWindow") {
    $candidate = Capture-PrintWindow $handle $rect
    if ($candidate -and (Test-BitmapHasContent $candidate)) {
        $bitmap = $candidate
        $usedMethod = "PrintWindow"
    } elseif ($candidate) {
        $candidate.Dispose()
    }
}

if (-not $bitmap -and ($Method -eq "Auto" -or $Method -eq "Screen")) {
    $bitmap = Capture-Screen $handle $rect ($BringToFront -or $Method -eq "Screen" -or $Method -eq "Auto")
    $usedMethod = "Screen"
}

if (-not $bitmap) {
    throw "Capture failed."
}

try {
    $saved = Save-Bitmap $bitmap $OutPath
    [pscustomobject]@{
        Path = $saved
        Method = $usedMethod
        ProcessName = $process.ProcessName
        ProcessId = $process.Id
        Title = $process.MainWindowTitle
        WindowHandle = $handle.ToInt64()
        Width = $rect.Width
        Height = $rect.Height
        X = $rect.X
        Y = $rect.Y
    } | ConvertTo-Json -Compress
} finally {
    $bitmap.Dispose()
}
