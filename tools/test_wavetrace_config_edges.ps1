[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot "build_vs\wavetrace_config_edge_tests"
$runtimeJson = Join-Path $buildRoot "runtime.json"
$generated = Join-Path $buildRoot "generated"
$fixture = Join-Path $repoRoot "tests\reflectgen_waveptr_config\input.hpp"
$reflectGen = Join-Path $repoRoot "tools\bin\wavetrace_reflectgen.exe"
$probe = Join-Path $repoRoot "build_vs\smoke_wavetrace_config_probe\Release\smoke_wavetrace_config_probe.exe"
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Write-Utf8([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-Probe([string]$Json, [int]$ExpectedExit, [string[]]$Contains) {
    Write-Utf8 $runtimeJson $Json
    $output = @(& $probe 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
    Assert-True ($LASTEXITCODE -eq $ExpectedExit) "probe exit=$LASTEXITCODE expected=$ExpectedExit output=$output"
    foreach ($needle in $Contains) {
        Assert-True ($output.Contains($needle)) "probe output missing '$needle': $output"
    }
}

if (Test-Path $buildRoot) {
    $resolved = [IO.Path]::GetFullPath($buildRoot)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repoRoot "build_vs"))
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clear unexpected path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $generated | Out-Null

& $msbuild (Join-Path $repoRoot "smoke_wavetrace_config_probe.vcxproj") /m /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "config probe build failed"

Write-Host "[1/7] Empty/null/default and numeric values"
Invoke-Probe '{"WaveTrace":true,"WaveTraceFileName":"a.wvz4","WaveTraceStart":"","WaveTraceEnd":null,"WaveTraceLevel":""}' 0 @('enabled=1','file=a.wvz4','start=0','end=18446744073709551615','level_enabled=0')
Invoke-Probe '{"WaveTrace":false,"WaveTraceFileName":"b.wvz4","WaveTraceStart":12,"WaveTraceEnd":"34","WaveTraceLevel":5,"WaveTraceDirtyArrayStats":true,"WaveTraceDirtyArrayMarks":true,"WaveTraceMemoryUsage":true}' 0 @('enabled=0','start=12','end=34','level_enabled=1','level=5','stats=1','marks=1','memory=1')

Write-Host "[2/7] Invalid ranges and overflow fail closed"
Invoke-Probe '{"WaveTraceStart":9,"WaveTraceEnd":8}' 2 @('WaveTraceEnd is smaller')
Invoke-Probe '{"WaveTraceStart":"18446744073709551616"}' 2 @('invalid WaveTrace config value')

Write-Host "[3/7] Malformed JSON must not silently use defaults"
Invoke-Probe '{ definitely-not-json' 2 @('invalid')

Write-Host "[4/7] Unicode escape is valid JSON"
$unicodeFileNeedle = 'file=' + [char]0x6ce2 + [char]0x5f62 + '.wvz4'
Invoke-Probe '{"WaveTraceFileName":"\u6ce2\u5f62.wvz4"}' 0 @($unicodeFileNeedle)

Write-Host "[5/7] WaveTrace=false placeholder headers compile"
$reflectConfig = Join-Path $buildRoot "wavetrace_config.json"
Write-Utf8 $reflectConfig '{"WaveTrace":false,"WaveTraceFileName":"off.wvz4","WaveTraceStart":"","WaveTraceEnd":"","wave_ptr_members":[]}'
& $reflectGen --reflect-root-class Root --batch-dir (Split-Path -Parent $fixture) --no-recursive --wavetrace-config $reflectConfig -o $generated --aggregate-header project_reflect_auto.h -- -x c++ -std=c++14 | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "disabled ReflectGen invocation failed"
& $msbuild (Join-Path $repoRoot "smoke_wavetrace_placeholder_include.vcxproj") /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "placeholder aggregate failed to compile"

Write-Host "[6/7] false -> true -> false removes stale aggregate reflection"
$falseClosure = Get-Content (Join-Path $generated "root_class_closure_reflect_auto.h") -Raw
Assert-True (-not $falseClosure.Contains('ReflectAccess<')) "disabled closure contains reflection"
Write-Utf8 $reflectConfig '{"WaveTrace":true,"WaveTraceFileName":"on.wvz4","WaveTraceStart":"","WaveTraceEnd":"","wave_ptr_members":[]}'
& $reflectGen --reflect-root-class Root --batch-dir (Split-Path -Parent $fixture) --no-recursive --wavetrace-config $reflectConfig -o $generated --aggregate-header project_reflect_auto.h -- -x c++ -std=c++14 | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "enabled ReflectGen invocation failed"
$trueClosure = Get-Content (Join-Path $generated "root_class_closure_reflect_auto.h") -Raw
Assert-True ($trueClosure.Contains('ReflectAccess<struct Root>')) "enabled closure did not contain Root"
$configObject = Get-Content $reflectConfig -Raw | ConvertFrom-Json
$configObject.WaveTrace = $false
Write-Utf8 $reflectConfig ($configObject | ConvertTo-Json -Depth 10)
& $reflectGen --reflect-root-class Root --batch-dir (Split-Path -Parent $fixture) --no-recursive --wavetrace-config $reflectConfig -o $generated --aggregate-header project_reflect_auto.h -- -x c++ -std=c++14 | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "second disabled ReflectGen invocation failed"
$falseAgain = Get-Content (Join-Path $generated "root_class_closure_reflect_auto.h") -Raw
Assert-True (-not $falseAgain.Contains('ReflectAccess<')) "false-after-true left stale aggregate reflection"

Write-Host "[7/7] WavePtr table survives disabled fast path"
$finalConfig = Get-Content $reflectConfig -Raw | ConvertFrom-Json
Assert-True (@($finalConfig.wave_ptr_members).Count -gt 0) "disabled pass discarded discovered WavePtr entries"

Write-Host "PASS: WaveTrace config edge cases passed"
