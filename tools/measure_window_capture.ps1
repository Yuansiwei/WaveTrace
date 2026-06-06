param(
    [string]$ProcessName,
    [string]$TitleRegex,
    [int]$Iterations = 30,
    [int]$Warmup = 5,
    [switch]$BringToFront
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$source = @"
using System;
using System.Runtime.InteropServices;

public static class WinCaptureMeasureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

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

if (-not ("WinCaptureMeasureNative" -as [type])) {
    Add-Type -TypeDefinition $source
}

function Get-TargetProcess {
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
    $rect = New-Object WinCaptureMeasureNative+RECT
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type]"WinCaptureMeasureNative+RECT")
    $hr = [WinCaptureMeasureNative]::DwmGetWindowAttribute(
        $Handle,
        [WinCaptureMeasureNative]::DWMWA_EXTENDED_FRAME_BOUNDS,
        [ref]$rect,
        $size)

    if ($hr -ne 0) {
        if (-not [WinCaptureMeasureNative]::GetWindowRect($Handle, [ref]$rect)) {
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

$process = Get-TargetProcess
$handle = [IntPtr]$process.MainWindowHandle

if ([WinCaptureMeasureNative]::IsIconic($handle)) {
    [void][WinCaptureMeasureNative]::ShowWindow($handle, [WinCaptureMeasureNative]::SW_RESTORE)
    Start-Sleep -Milliseconds 200
}
if ($BringToFront) {
    [void][WinCaptureMeasureNative]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 250
}

$rect = Get-CaptureRectangle $handle
$bitmap = New-Object System.Drawing.Bitmap($rect.Width, $rect.Height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
$samples = New-Object 'System.Collections.Generic.List[double]'

try {
    $total = [Math]::Max(0, $Warmup) + [Math]::Max(1, $Iterations)
    for ($i = 0; $i -lt $total; ++$i) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $ok = [WinCaptureMeasureNative]::PrintWindow($handle, $hdc, [WinCaptureMeasureNative]::PW_RENDERFULLCONTENT)
        $sw.Stop()
        if (-not $ok) {
            throw "PrintWindow failed."
        }
        if ($i -ge $Warmup) {
            $samples.Add($sw.Elapsed.TotalMilliseconds)
        }
    }
} finally {
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $bitmap.Dispose()
}

$ordered = @($samples | Sort-Object)
$count = $ordered.Count
$sum = 0.0
foreach ($value in $ordered) { $sum += [double]$value }

function Percentile([double[]]$Values, [double]$P) {
    if ($Values.Count -eq 0) { return $null }
    $index = [Math]::Min($Values.Count - 1, [Math]::Max(0, [int][Math]::Floor(($Values.Count - 1) * $P)))
    return $Values[$index]
}

[pscustomobject]@{
    ProcessName = $process.ProcessName
    ProcessId = $process.Id
    Title = $process.MainWindowTitle
    Width = $rect.Width
    Height = $rect.Height
    Iterations = $count
    AverageMs = [Math]::Round($sum / $count, 3)
    MedianMs = [Math]::Round((Percentile $ordered 0.50), 3)
    P90Ms = [Math]::Round((Percentile $ordered 0.90), 3)
    MinMs = [Math]::Round($ordered[0], 3)
    MaxMs = [Math]::Round($ordered[$count - 1], 3)
} | ConvertTo-Json -Compress
