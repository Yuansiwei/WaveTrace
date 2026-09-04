[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot "build_vs\wavetrace_window_tests"
$runtimeJson = Join-Path $buildRoot "runtime.json"
$writer = Join-Path $repoRoot "build_vs\smoke_wavetrace_window_writer\Release\smoke_wavetrace_window_writer.exe"
$parser = Join-Path $repoRoot "QtViewer\build\x64\Release\smoke_wvz4_range_parser.exe"
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Write-Config([bool]$Enabled, [string]$FileName, $Start, $End) {
    $config = [ordered]@{
        WaveTrace = $Enabled
        WaveTraceFileName = $FileName
        WaveTraceStart = $Start
        WaveTraceEnd = $End
        WaveTraceLevel = ""
        wave_ptr_members = @()
    }
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    [IO.File]::WriteAllText($runtimeJson, ($config | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}

function Run-Writer([int]$Cycles, [string]$OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath ($OutputPath + '.writer.log') -Force -ErrorAction SilentlyContinue
    & $writer $Cycles | Out-Host
    Assert-True ($LASTEXITCODE -eq 0) "writer failed for $OutputPath"
}

function Check-Range([string]$Path, [long]$Start, [long]$End, [long]$LastAny, [long]$LastNonClock) {
    & $parser $Path $Start $End $LastAny $LastNonClock | Out-Host
    Assert-True ($LASTEXITCODE -eq 0) "range parser failed for $Path"
}

if (Test-Path $buildRoot) {
    $resolved = [IO.Path]::GetFullPath($buildRoot)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repoRoot "build_vs"))
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clear unexpected path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

& $msbuild (Join-Path $repoRoot "smoke_wavetrace_window_writer.vcxproj") /m /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "window writer build failed"
& $msbuild (Join-Path $repoRoot "QtViewer\smoke_wvz4_range_parser.vcxproj") /m /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "range parser build failed"

$legacy = Join-Path $repoRoot "build_vs\legacy_name_must_not_exist.wvz4"
Remove-Item -LiteralPath $legacy -Force -ErrorAction SilentlyContinue

Write-Host "[1/6] Finite window with default clock"
$file = Join-Path $buildRoot "finite.wvz4"
Write-Config $true ($file.Replace('\','/')) 2 3
Run-Writer 6 $file
Check-Range $file 20 40 40 30
Assert-True (-not (Test-Path $legacy)) "legacy OpenConfig filename overrode JSON"

Write-Host "[2/6] Single-cycle window"
$file = Join-Path $buildRoot "single.wvz4"
Write-Config $true ($file.Replace('\','/')) 3 3
Run-Writer 6 $file
Check-Range $file 30 40 40 30

Write-Host "[3/6] Simulation ends before configured end"
$file = Join-Path $buildRoot "short.wvz4"
Write-Config $true ($file.Replace('\','/')) 2 100
Run-Writer 5 $file
Check-Range $file 20 50 50 40

Write-Host "[4/6] Start after simulation end creates no file"
$file = Join-Path $buildRoot "not_reached.wvz4"
Write-Config $true ($file.Replace('\','/')) 10 12
Run-Writer 5 $file
Assert-True (-not (Test-Path $file)) "unreached trace window created a waveform"

Write-Host "[5/6] Empty bounds use zero and actual end"
$file = Join-Path $buildRoot "unbounded.wvz4"
Write-Config $true ($file.Replace('\','/')) "" ""
Run-Writer 3 $file
Check-Range $file 0 30 30 20

Write-Host "[6/6] Disabled mode and Unicode filename"
$file = Join-Path $buildRoot "disabled.wvz4"
Write-Config $false ($file.Replace('\','/')) "" ""
Run-Writer 3 $file
Assert-True (-not (Test-Path $file)) "WaveTrace=false created a waveform"
$unicodeName = [char]0x6ce2 + [char]0x5f62 + '.wvz4'
$file = Join-Path $buildRoot $unicodeName
Write-Config $true ($file.Replace('\','/')) 1 1
Run-Writer 3 $file
Check-Range $file 10 20 20 10

Write-Host "PASS: WaveTrace runtime windows passed"
