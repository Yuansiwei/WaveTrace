param(
    [int[]]$ShardCounts = @(0, 1, 8, 32),
    [int]$BulkElements = 4096,
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$probeSource = Join-Path $repo 'tests\cmake\shard_signal_matrix_probe'
$inputHeader = Join-Path $probeSource 'signal_matrix_input.h'
$reflectGen = Join-Path $repo 'tools\bin\wavetrace_reflectgen.exe'
$runner = Join-Path $repo 'cmake\run_reflectgen.cmake'
$testRoot = Join-Path $repo 'build\shard_signal_matrix'

if (-not (Test-Path -LiteralPath $reflectGen -PathType Leaf)) {
    throw "ReflectGen executable not found: $reflectGen"
}
if ($BulkElements -le 0) {
    throw 'BulkElements must be positive.'
}

New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$snapshots = @{}

foreach ($shards in $ShardCounts) {
    if ($shards -lt 0 -or $shards -gt 64) {
        throw "Invalid shard count: $shards"
    }

    $caseRoot = Join-Path $testRoot "shards_$shards"
    $generated = Join-Path $caseRoot 'generated_reflect'
    $buildDir = Join-Path $caseRoot 'build'
    $targets = Join-Path $caseRoot 'targets.txt'
    $config = Join-Path $caseRoot 'wavetrace_config.json'
    $settings = Join-Path $caseRoot 'wavetrace_reflection.settings'
    $snapshot = Join-Path $caseRoot 'signals.snapshot'
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null

    $inputForCMake = $inputHeader -replace '\\', '/'
    Set-Content -LiteralPath $targets -Encoding Ascii -Value ('"' + $inputForCMake + '"')
    Set-Content -LiteralPath $config -Encoding Ascii -Value @"
{
  "WaveTrace": true,
  "WaveTraceFileName": "signal_matrix.wvz4",
  "WaveTraceStart": "",
  "WaveTraceEnd": "",
  "wave_ptr_members": []
}
"@
    Set-Content -LiteralPath $settings -Encoding Ascii -Value "compile_shards=$shards"

    $reflectArgs = @(
        "-DWAVETRACE_REFLECTGEN_EXE=$reflectGen",
        "-DWAVETRACE_REFLECT_TARGET_LIST=$targets",
        '-DWAVETRACE_REFLECT_ROOT_CLASS=ShardSignalMatrixRoot',
        "-DWAVETRACE_REFLECT_COMPILE_SHARDS=$shards",
        "-DWAVETRACE_REFLECT_OUTPUT_DIR=$generated",
        '-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h',
        "-DWAVETRACE_REFLECT_LOG_FILE=$generated\reflectgen.log",
        "-DWAVETRACE_CONFIG_FILE=$config",
        "-DWAVETRACE_REFLECT_SETTINGS_FILE=$settings",
        "-DWAVETRACE_REFLECT_CLANG_ARGS=-I$repo",
        '-P', $runner
    )
    & cmake @reflectArgs
    if ($LASTEXITCODE -ne 0) { throw "ReflectGen failed for shard count $shards" }

    $configureArgs = @(
        '-S', $probeSource,
        '-B', $buildDir,
        '-A', 'x64',
        "-DWAVETRACE_ROOT=$repo",
        "-DSHARD_DIR=$generated",
        "-DSHARD_COUNT=$shards"
    )
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for shard count $shards" }

    & cmake --build $buildDir --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed for shard count $shards" }

    $exe = Join-Path $buildDir "$Configuration\wavetrace_shard_signal_matrix_probe.exe"
    & $exe $snapshot $BulkElements
    if ($LASTEXITCODE -ne 0) { throw "Signal matrix probe failed for shard count $shards" }
    $snapshots[$shards] = $snapshot
}

$baselineShardCount = $ShardCounts[0]
$baseline = $snapshots[$baselineShardCount]
foreach ($shards in $ShardCounts) {
    if ($shards -eq $baselineShardCount) { continue }
    $difference = Compare-Object -ReferenceObject (Get-Content -LiteralPath $baseline) -DifferenceObject (Get-Content -LiteralPath $snapshots[$shards])
    if ($difference) {
        $preview = $difference | Select-Object -First 40 | Out-String
        throw "Signal set differs: baseline=$baselineShardCount shards=$shards`n$preview"
    }
}

$hash = (Get-FileHash -LiteralPath $baseline -Algorithm SHA256).Hash
Write-Host "strict shard signal matrix passed: shards=$($ShardCounts -join ',') elements=$BulkElements sha256=$hash"
