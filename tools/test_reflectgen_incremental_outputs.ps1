[CmdletBinding()]
param(
    [string]$ReflectGenExe = "",
    [string]$BuildRoot = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ReflectGenExe)) {
    $ReflectGenExe = Join-Path $repoRoot "tools\bin\wavetrace_reflectgen.exe"
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repoRoot "build_vs\reflectgen_incremental_output_tests"
}
$ReflectGenExe = [IO.Path]::GetFullPath($ReflectGenExe)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$fixture = Join-Path $repoRoot "tests\reflectgen_waveptr_config\input.hpp"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-ReflectGen([string[]]$Arguments) {
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $lines = @(& $ReflectGenExe @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($exitCode -ne 0) { throw "ReflectGen failed ($exitCode):`n$($lines -join "`n")" }
    return $lines -join "`n"
}

function Snapshot-GeneratedFiles([string]$Directory) {
    $snapshot = @{}
    Get-ChildItem -LiteralPath $Directory -File | Where-Object {
        $_.Name -ne "reflectgen.log"
    } | ForEach-Object {
        $snapshot[$_.FullName] = @{
            Ticks = $_.LastWriteTimeUtc.Ticks
            Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    }
    return $snapshot
}

function Assert-SnapshotUnchanged($Snapshot) {
    foreach ($path in $Snapshot.Keys) {
        $item = Get-Item -LiteralPath $path
        Assert-True ($item.LastWriteTimeUtc.Ticks -eq $Snapshot[$path].Ticks) "timestamp changed: $path"
        Assert-True ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $Snapshot[$path].Hash) "content changed: $path"
    }
}

if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$inputDir = Join-Path $BuildRoot "input"
New-Item -ItemType Directory -Force -Path $inputDir | Out-Null
$input = Join-Path $inputDir "input.hpp"
Copy-Item -LiteralPath $fixture -Destination $input -Force
$config = Join-Path $BuildRoot "wavetrace_config.json"
Write-Utf8NoBom $config '{"WaveTrace":true,"wave_ptr_members":[]}'

Write-Host "[1/4] Single generated header remains untouched when content is identical"
$single = Join-Path $BuildRoot "single\input_reflect.h"
$singleArgs = @($input, "-o", $single, "--header-include", "input.hpp",
    "--wavetrace-config", $config, "--main-file-only", "--", "-x", "c++", "-std=c++14")
Invoke-ReflectGen $singleArgs | Out-Null
$singleText = Get-Content -LiteralPath $single -Raw
Assert-True ($singleText -match 'ordinary_raw_ptr.*PointerOrReferenceFieldTag') "ordinary raw pointer was not tagged"
Assert-True ($singleText -match 'ordinary_reference.*PointerOrReferenceFieldTag') "ordinary reference was not tagged"
$singleSnapshot = Snapshot-GeneratedFiles (Split-Path -Parent $single)
Start-Sleep -Milliseconds 1100
$singleLog = Invoke-ReflectGen $singleArgs
Assert-SnapshotUnchanged $singleSnapshot
Assert-True $singleLog.Contains("[generated output] unchanged:") "single rerun did not report unchanged output"

Write-Host "[2/4] Batch headers, shard inputs, registry, and shard sources remain untouched"
$batch = Join-Path $BuildRoot "batch\generated_reflect"
$batchArgs = @("--reflect-root-class", "Root", "--batch-dir", $inputDir,
    "--no-recursive", "--compile-shards", "3", "--wavetrace-config", $config,
    "-o", $batch, "--aggregate-header", "project_reflect_auto.h",
    "--", "-x", "c++", "-std=c++14")
Invoke-ReflectGen $batchArgs | Out-Null
$batchSnapshot = Snapshot-GeneratedFiles $batch
Start-Sleep -Milliseconds 1100
$batchLog = Invoke-ReflectGen $batchArgs
Assert-SnapshotUnchanged $batchSnapshot
Assert-True $batchLog.Contains("[generated output] unchanged:") "batch rerun did not report unchanged output"
Assert-True (@(Get-ChildItem -LiteralPath $batch -Filter "*.tmp.*" -File).Count -eq 0) "temporary outputs leaked"

Write-Host "[3/4] Changed reflection content replaces the affected output"
$closure = Join-Path $batch "root_class_closure_reflect_auto.h"
$closureTicks = (Get-Item -LiteralPath $closure).LastWriteTimeUtc.Ticks
$source = Get-Content -LiteralPath $input -Raw
$source = $source.Replace("struct Root {", "struct Root {`r`n    int incremental_output_probe;")
Write-Utf8NoBom $input $source
Start-Sleep -Milliseconds 1100
$changedLog = Invoke-ReflectGen $batchArgs
Assert-True ((Get-Item -LiteralPath $closure).LastWriteTimeUtc.Ticks -ne $closureTicks) "changed reflection did not replace closure header"
Assert-True ((Get-Content -LiteralPath $closure -Raw).Contains("incremental_output_probe")) "changed member missing from closure header"
Assert-True $changedLog.Contains("[generated output] updated:") "changed output did not report update"
foreach ($path in $batchSnapshot.Keys) {
    if ($path -eq $closure) { continue }
    Assert-True ((Get-Item -LiteralPath $path).LastWriteTimeUtc.Ticks -eq $batchSnapshot[$path].Ticks) "unaffected generated output was touched: $path"
}

Write-Host "[4/4] WaveTrace=false outputs also remain untouched"
Write-Utf8NoBom $config '{"WaveTrace":false,"wave_ptr_members":[]}'
$disabled = Join-Path $BuildRoot "disabled\generated_reflect"
$disabledArgs = @("--reflect-root-class", "Root", "--batch-dir", $inputDir,
    "--no-recursive", "--compile-shards", "3", "--wavetrace-config", $config,
    "-o", $disabled, "--aggregate-header", "project_reflect_auto.h",
    "--", "-x", "c++", "-std=c++14")
Invoke-ReflectGen $disabledArgs | Out-Null
$disabledSnapshot = Snapshot-GeneratedFiles $disabled
Start-Sleep -Milliseconds 1100
Invoke-ReflectGen $disabledArgs | Out-Null
Assert-SnapshotUnchanged $disabledSnapshot

Write-Host "ReflectGen incremental output regression passed."
