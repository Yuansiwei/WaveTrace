[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $repoRoot "build_vs\wavetrace_build_integration_tests"
$sourceRoot = Join-Path $testRoot "source"
$packageRoot = Join-Path $sourceRoot "WaveTracer"
$generated = Join-Path $packageRoot "generated_reflect"
$buildRoot = Join-Path $testRoot "build"
$config = Join-Path $packageRoot "wavetrace_config.json"
$targetList = Join-Path $sourceRoot "targets.txt"
$fixture = Join-Path $repoRoot "tests\reflectgen_waveptr_config\input.hpp"
$runner = Join-Path $repoRoot "cmake\run_reflectgen.cmake"
$reflectGen = Join-Path $repoRoot "tools\bin\wavetrace_reflectgen.exe"
$helper = Join-Path $repoRoot "tools\bin\wvz4_writer_monitor.exe"
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Write-Utf8([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

if (Test-Path $testRoot) {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repoRoot "build_vs"))
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clear unexpected path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $generated,$buildRoot | Out-Null
Write-Utf8 $targetList ('"' + $fixture.Replace('\','/') + '"' + "`n")
Write-Utf8 $config '{"WaveTrace":true,"WaveTraceFileName":"source.wvz4","WaveTraceStart":"","WaveTraceEnd":"","wave_ptr_members":[]}'

$cmakeArgs = @(
    "-DWAVETRACE_REFLECTGEN_EXE=$($reflectGen.Replace('\','/'))",
    "-DWAVETRACE_REFLECT_ROOT_CLASS=Root",
    "-DWAVETRACE_REFLECT_OUTPUT_DIR=$($generated.Replace('\','/'))",
    "-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h",
    "-DWAVETRACE_REFLECT_LOG_FILE=$($generated.Replace('\','/'))/reflectgen.log",
    "-DWAVETRACE_REFLECT_TARGET_LIST=$($targetList.Replace('\','/'))",
    "-DWAVETRACE_CONFIG_FILE=$($config.Replace('\','/'))",
    "-DWAVETRACE_REFLECT_CLANG_ARGS=-x;c++;-std=c++14",
    "-P", $runner
)

Write-Host "[1/5] CMake runner writes JSON/log/headers only under source WaveTracer"
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "CMake runner failed"
$aggregate = Join-Path $generated "project_reflect_auto.h"
$closure = Join-Path $generated "root_class_closure_reflect_auto.h"
$log = Join-Path $generated "reflectgen.log"
Assert-True ((Test-Path $config) -and (Test-Path $aggregate) -and (Test-Path $closure) -and (Test-Path $log)) "source outputs are incomplete"
Assert-True (-not (Test-Path (Join-Path $buildRoot "WaveTracer"))) "runner created a build-tree WaveTracer output"
Assert-True ((Get-Content $closure -Raw).Contains('ReflectAccess<struct Root>')) "enabled runner output lacks Root reflection"

Write-Host "[2/5] CMake runner executes ReflectGen without touching identical generated outputs"
$firstStamp = (Get-Item $aggregate).LastWriteTimeUtc
$firstClosureStamp = (Get-Item $closure).LastWriteTimeUtc
$firstLogStamp = (Get-Item $log).LastWriteTimeUtc
Start-Sleep -Milliseconds 1200
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "incremental CMake runner failed"
Assert-True ((Get-Item $log).LastWriteTimeUtc -gt $firstLogStamp) "unchanged runner did not execute ReflectGen"
Assert-True ((Get-Item $aggregate).LastWriteTimeUtc -eq $firstStamp) "unchanged runner touched aggregate header"
Assert-True ((Get-Item $closure).LastWriteTimeUtc -eq $firstClosureStamp) "unchanged runner touched closure header"

Write-Host "[3/5] ReflectGen owns the WaveTrace=false decision"
$enabledStamp = (Get-Item $closure).LastWriteTimeUtc
$json = Get-Content $config -Raw | ConvertFrom-Json
$json.WaveTrace = $false
Write-Utf8 $config ($json | ConvertTo-Json -Depth 10)
Start-Sleep -Milliseconds 1200
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "disabled CMake runner failed"
Assert-True (-not (Get-Content $closure -Raw).Contains('ReflectAccess<struct Root>')) "WaveTrace=false did not emit empty reflection"
$disabledStamp = (Get-Item $closure).LastWriteTimeUtc
Assert-True ($disabledStamp -gt $enabledStamp) "disabled runner did not execute ReflectGen"
Start-Sleep -Milliseconds 1200
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "settled disabled CMake runner failed"
Assert-True ((Get-Item $closure).LastWriteTimeUtc -eq $disabledStamp) "settled disabled runner touched unchanged output"

Write-Host "[4/5] MSBuild props resolve source paths and C++-safe slashes"
$propsPath = (Join-Path $repoRoot "props\wavetrace_reflectgen_reference.props").Replace('\','/')
$dumpFile = Join-Path $testRoot "props_values.txt"
$itemsDumpFile = Join-Path $testRoot "props_items.txt"
$probeProject = Join-Path $testRoot "props_probe.proj"
$projectXml = @"
<Project DefaultTargets="Dump" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup><WaveTraceRoot>$($packageRoot.Replace('\','/'))</WaveTraceRoot></PropertyGroup>
  <Import Project="$propsPath" />
  <Target Name="Dump">
    <WriteLinesToFile File="$($dumpFile.Replace('\','/'))" Overwrite="true" Lines="`$(WaveTraceReflectGenDir)|`$(WaveTraceConfigFile)|`$(WaveTraceReflectLogFile)|`$(WaveTraceConfigPathForCpp)" />
    <WriteLinesToFile File="$($itemsDumpFile.Replace('\','/'))" Overwrite="true" Lines="@(None->'%(Identity)|%(Link)')" />
  </Target>
</Project>
"@
Write-Utf8 $probeProject $projectXml
& $msbuild $probeProject /t:Dump /nologo /verbosity:minimal | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "MSBuild props probe failed"
$values = (Get-Content $dumpFile -Raw).Trim()
$parts = $values.Split('|')
Assert-True ($parts.Count -eq 4) "unexpected props probe output: $values"
Assert-True ($parts[0].Replace('\','/').StartsWith($packageRoot.Replace('\','/'))) "generated dir is not under source package: $($parts[0])"
Assert-True ($parts[1].Replace('\','/') -eq $config.Replace('\','/')) "config path is not source config: $($parts[1])"
Assert-True ($parts[2].Replace('\','/').StartsWith($generated.Replace('\','/'))) "log path is not source generated dir: $($parts[2])"
Assert-True (-not $parts[3].Contains('\')) "C++ config macro path still contains backslashes: $($parts[3])"
$projectItems = Get-Content $itemsDumpFile -Raw
Assert-True ($projectItems.Contains('WaveTrace\wavetrace_config.json')) "JSON config is not visible as a WaveTrace project item"

Write-Host "[5/5] Old writer-helper protocol is rejected before pipe startup"
$savedErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $oldProtocolOutput = @(& $helper --writer-helper --pipe old_protocol_probe --parent-pid $PID --out ignored.wvz4 --ipc-version 2 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
    $oldProtocolExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $savedErrorAction
}
Assert-True ($oldProtocolExit -eq 2) "old helper protocol exit=$oldProtocolExit output=$oldProtocolOutput"
Assert-True ($oldProtocolOutput.Contains('protocol mismatch')) "old helper protocol was not diagnosed: $oldProtocolOutput"

Write-Host "PASS: WaveTrace build integration passed"
