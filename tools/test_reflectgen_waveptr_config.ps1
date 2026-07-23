[CmdletBinding()]
param(
    [string]$ReflectGenExe = "",
    [string]$BuildRoot = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureDir = Join-Path $repoRoot "tests\reflectgen_waveptr_config"

if ([string]::IsNullOrWhiteSpace($ReflectGenExe)) {
    $ReflectGenExe = Join-Path $repoRoot "tools\bin\wavetrace_reflectgen.exe"
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repoRoot "build_vs\reflectgen_waveptr_config_tests"
}

$ReflectGenExe = [IO.Path]::GetFullPath($ReflectGenExe)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$inputHeader = Join-Path $fixtureDir "input.hpp"
$runScript = Join-Path $repoRoot "cmake\run_reflectgen.cmake"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Invoke-ReflectGen {
    param(
        [string[]]$Arguments,
        [int]$ExpectedExitCode = 0
    )
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $lines = @(& $ReflectGenExe @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }
    $text = $lines -join "`n"
    if ($exitCode -ne $ExpectedExitCode) {
        throw "ReflectGen exit code $exitCode, expected $ExpectedExitCode.`n$text"
    }
    return $text
}

function Read-ConfigEntries {
    param([string]$Path)
    $json = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    return @($json.wave_ptr_members)
}

function Find-Entry {
    param($Entries, [string]$ClassName, [string]$MemberName)
    return @($Entries | Where-Object { $_.class -eq $ClassName -and $_.member -eq $MemberName })
}

function Assert-GeneratedMember {
    param([string]$Text, [string]$MemberName, [bool]$Expected)
    $found = $Text.Contains('"' + $MemberName + '"')
    Assert-True ($found -eq $Expected) "generated member '$MemberName' expected=$Expected actual=$found"
}

function New-CompleteConfigText {
    param([bool]$TemplatePtr, [bool]$DirectPtr)
    $templateValue = if ($TemplatePtr) { "true" } else { "false" }
    $directValue = if ($DirectPtr) { "true" } else { "false" }
    return @"
{
  "wave_ptr_members": [
    {"class": "OldClass", "member": "old_ptr", "reflect": false},
    {"class": "Root", "member": "alias_ptr", "reflect": true},
    {"class": "Root", "member": "annotated_array", "reflect": true},
    {"class": "Root", "member": "annotated_ptr", "reflect": true},
    {"class": "Root", "member": "annotated_shared", "reflect": true},
    {"class": "Root", "member": "annotated_weak", "reflect": true},
    {"class": "Root", "member": "direct_ptr", "reflect": $directValue},
    {"class": "alpha::Box", "member": "enabled_ptr", "reflect": true},
    {"class": "alpha::Box", "member": "template_ptr", "reflect": $templateValue},
    {"class": "beta::Box", "member": "other_ptr", "reflect": true}
  ]
}
"@
}

if (-not (Test-Path -LiteralPath $ReflectGenExe -PathType Leaf)) {
    throw "ReflectGen executable not found: $ReflectGenExe"
}
if (-not (Test-Path -LiteralPath $inputHeader -PathType Leaf)) {
    throw "Fixture header not found: $inputHeader"
}

if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$singleRoot = Join-Path $BuildRoot "single\WaveTracer"
$singleOut = Join-Path $singleRoot "generated_reflect\input_reflect.h"
$singleConfig = Join-Path $singleRoot "wavetrace_config.json"

Write-Host "[1/12] Missing entries default to true; templates and aliases are discovered"
$freshLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--header-include", "input.hpp",
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
)
$entries = Read-ConfigEntries $singleConfig
Assert-True ($entries.Count -eq 9) "expected exactly nine unique pointer member entries, got $($entries.Count)"
$freshConfigText = Get-Content -LiteralPath $singleConfig -Raw
Assert-True (-not $freshConfigText.Contains('"WaveTraceDirtyArrayStats"')) "default false DirtyArrayStats should be omitted"
Assert-True (-not $freshConfigText.Contains('"WaveTraceDirtyArrayMarks"')) "default false DirtyArrayMarks should be omitted"
Assert-True (-not $freshConfigText.Contains('"WaveTraceMemoryUsage"')) "default false MemoryUsage should be omitted"
Assert-True (@(Find-Entry $entries "alpha::Box" "template_ptr").Count -eq 1) "template instances must share alpha::Box::template_ptr"
Assert-True (@(Find-Entry $entries "Root" "alias_ptr").Count -eq 1) "annotated pointer alias field was not discovered"
foreach ($entry in $entries) {
    Assert-True ([bool]$entry.reflect) "new entry $($entry.class)::$($entry.member) did not default to true"
}
$singleText = Get-Content -LiteralPath $singleOut -Raw
foreach ($member in @("template_ptr", "enabled_ptr", "other_ptr", "direct_ptr", "alias_ptr",
                      "annotated_ptr", "annotated_array", "annotated_shared", "annotated_weak")) {
    Assert-GeneratedMember $singleText $member $true
}

Write-Host "[2/12] false remains generated for runtime filtering"
Write-Utf8NoBom $singleConfig (New-CompleteConfigText -TemplatePtr $false -DirectPtr $false)
$disabledLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--header-include", "input.hpp",
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
)
$singleText = Get-Content -LiteralPath $singleOut -Raw
Assert-GeneratedMember $singleText "template_ptr" $true
Assert-GeneratedMember $singleText "direct_ptr" $true
Assert-GeneratedMember $singleText "other_ptr" $true
Assert-GeneratedMember $singleText "alias_ptr" $true
$entries = Read-ConfigEntries $singleConfig
$oldEntries = @(Find-Entry $entries "OldClass" "old_ptr")
Assert-True ($oldEntries.Count -eq 1 -and $oldEntries[0].reflect -eq $false) "stale explicit false entry was not preserved"

Write-Host "[3/12] Batch root-closure mode retains runtime-switchable members"
$batchInputDir = Join-Path $BuildRoot "batch_input"
New-Item -ItemType Directory -Force -Path $batchInputDir | Out-Null
Copy-Item -LiteralPath $inputHeader -Destination (Join-Path $batchInputDir "input.hpp") -Force
$batchOut = Join-Path $BuildRoot "batch\WaveTracer\generated_reflect"
$batchLog = Invoke-ReflectGen @(
    "--reflect-root-class", "Root",
    "--batch-dir", $batchInputDir, "--no-recursive",
    "--wavetrace-config", $singleConfig,
    "-o", $batchOut, "--aggregate-header", "project_reflect_auto.h",
    "--", "-x", "c++", "-std=c++14"
)
$closureHeader = Join-Path $batchOut "root_class_closure_reflect_auto.h"
$closureText = Get-Content -LiteralPath $closureHeader -Raw
Assert-GeneratedMember $closureText "template_ptr" $true
Assert-GeneratedMember $closureText "direct_ptr" $true
Assert-GeneratedMember $closureText "enabled_ptr" $true
Assert-GeneratedMember $closureText "other_ptr" $true
Assert-True (-not $closureText.Contains("ReflectAccess<std::")) "root closure emitted ReflectAccess for a standard-library wrapper"
Assert-True (-not $closureText.Contains("topology_type_estimate<std::")) "root closure redefined a runtime-owned std topology estimate"
Assert-True (-not $closureText.Contains("reflected_visitor<std::")) "root closure emitted reflected_visitor for a standard-library wrapper"
Assert-True ($closureText.Contains("ReflectAccess<struct PayloadA>")) "std wrapper traversal did not retain PayloadA"
Assert-True ($closureText.Contains("ReflectAccess<struct PayloadB>")) "std wrapper traversal did not retain PayloadB"

Write-Host "[4/12] Malformed JSON fails before replacing generated output"
$beforeHash = (Get-FileHash -LiteralPath $singleOut -Algorithm SHA256).Hash
Write-Utf8NoBom $singleConfig '{"wave_ptr_members":['
$malformedLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
) 2
$afterHash = (Get-FileHash -LiteralPath $singleOut -Algorithm SHA256).Hash
Assert-True ($beforeHash -eq $afterHash) "malformed JSON changed an existing generated header"
Assert-True ($malformedLog.Contains("invalid JSON")) "malformed JSON did not produce a clear diagnostic"

Write-Host "[5/12] Duplicate class/member entries are rejected"
$duplicate = @"
{
  "wave_ptr_members": [
    {"class": "Root", "member": "direct_ptr", "reflect": true},
    {"class": "Root", "member": "direct_ptr", "reflect": false}
  ]
}
"@
Write-Utf8NoBom $singleConfig $duplicate
$duplicateLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
) 2
Assert-True ($duplicateLog.Contains("duplicate wave_ptr_members class/member entry")) "duplicate entry was not diagnosed"

Write-Host "[6/12] CMake runner always delegates policy to ReflectGen"
Write-Utf8NoBom $singleConfig (New-CompleteConfigText -TemplatePtr $false -DirectPtr $false)
$cmakeOut = (Join-Path $BuildRoot "cmake\WaveTracer\generated_reflect").Replace('\', '/')
$cmakeLog = "$cmakeOut/reflectgen.log"
$cmakeArgs = @(
    "-DWAVETRACE_REFLECTGEN_EXE=$($ReflectGenExe.Replace('\', '/'))",
    "-DWAVETRACE_REFLECT_ROOT_CLASS=Root",
    "-DWAVETRACE_REFLECT_OUTPUT_DIR=$cmakeOut",
    "-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h",
    "-DWAVETRACE_REFLECT_LOG_FILE=$cmakeLog",
    "-DWAVETRACE_REFLECT_BATCH_DIR=$($batchInputDir.Replace('\', '/'))",
    "-DWAVETRACE_CONFIG_FILE=$($singleConfig.Replace('\', '/'))",
    "-DWAVETRACE_REFLECT_CLANG_ARGS=-x;c++;-std=c++14",
    "-P", $runScript
)
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "first CMake runner invocation failed"
$aggregate = Join-Path $cmakeOut "project_reflect_auto.h"
$firstTime = (Get-Item -LiteralPath $aggregate).LastWriteTimeUtc
Start-Sleep -Milliseconds 1200
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "second CMake runner invocation failed"
$settledTime = (Get-Item -LiteralPath $aggregate).LastWriteTimeUtc
Assert-True ($settledTime -gt $firstTime) "second runner invocation did not execute ReflectGen"
Start-Sleep -Milliseconds 1200
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "third CMake runner invocation failed"
$unchangedTime = (Get-Item -LiteralPath $aggregate).LastWriteTimeUtc
Assert-True ($unchangedTime -gt $settledTime) "third runner invocation did not execute ReflectGen"

Start-Sleep -Milliseconds 1200
Write-Utf8NoBom $singleConfig (New-CompleteConfigText -TemplatePtr $false -DirectPtr $true)
& cmake @cmakeArgs | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "CMake runner failed after config change"
$changedTime = (Get-Item -LiteralPath $aggregate).LastWriteTimeUtc
Assert-True ($changedTime -gt $unchangedTime) "runtime-only config edit was filtered by the CMake runner"
$cmakeClosure = Get-Content -LiteralPath (Join-Path $cmakeOut "root_class_closure_reflect_auto.h") -Raw
Assert-GeneratedMember $cmakeClosure "direct_ptr" $true
Assert-GeneratedMember $cmakeClosure "template_ptr" $true

Write-Host "[7/12] Final deterministic rerun"
$settleLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
)
$configBefore = Get-Content -LiteralPath $singleConfig -Raw
$finalLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
)
$configAfter = Get-Content -LiteralPath $singleConfig -Raw
Assert-True ($configBefore -eq $configAfter) "deterministic rerun rewrote config contents"
Assert-True ($finalLog.Contains("unchanged:")) "deterministic rerun did not report unchanged config"

Write-Host "[8/12] A changed read-only config is forcibly replaced and remains read-only"
Write-Utf8NoBom $singleConfig '{"wave_ptr_members":[]}'
$configItem = Get-Item -LiteralPath $singleConfig
$configItem.IsReadOnly = $true
try {
    $readOnlyLog = Invoke-ReflectGen @(
        $inputHeader, "-o", $singleOut,
        "--header-include", "input.hpp",
        "--wavetrace-config", $singleConfig,
        "--main-file-only", "--", "-x", "c++", "-std=c++14"
    )
    $readOnlyEntries = Read-ConfigEntries $singleConfig
    Assert-True ($readOnlyEntries.Count -eq 9) "read-only config was not populated by forced replacement"
    Assert-True ((Get-Item -LiteralPath $singleConfig).IsReadOnly) "read-only attribute was not restored after replacement"
    Assert-True ($readOnlyLog.Contains("temporarily cleared protected attributes")) "read-only replacement path was not exercised"
}
finally {
    (Get-Item -LiteralPath $singleConfig).IsReadOnly = $false
}

Write-Host "[9/12] An exclusively opened config does not fail reflection or corrupt JSON"
Write-Utf8NoBom $singleConfig '{"wave_ptr_members":[]}'
$lockedBefore = Get-Content -LiteralPath $singleConfig -Raw
$lockedStream = [IO.File]::Open($singleConfig, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $lockedLog = Invoke-ReflectGen @(
        $inputHeader, "-o", $singleOut,
        "--header-include", "input.hpp",
        "--wavetrace-config", $singleConfig,
        "--main-file-only", "--", "-x", "c++", "-std=c++14"
    )
}
finally {
    $lockedStream.Dispose()
}
$lockedAfter = Get-Content -LiteralPath $singleConfig -Raw
Assert-True ($lockedAfter -eq $lockedBefore) "locked config was partially overwritten or corrupted"
Assert-True ($lockedLog.Contains("continuing with the discovered table in memory")) "locked config did not report the non-fatal in-memory fallback"
$lockedGenerated = Get-Content -LiteralPath $singleOut -Raw
Assert-GeneratedMember $lockedGenerated "direct_ptr" $true
Assert-GeneratedMember $lockedGenerated "template_ptr" $true

Write-Host "[10/12] The next unlocked run persists the previously discovered table"
$recoveryLog = Invoke-ReflectGen @(
    $inputHeader, "-o", $singleOut,
    "--header-include", "input.hpp",
    "--wavetrace-config", $singleConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
)
$recoveredEntries = Read-ConfigEntries $singleConfig
Assert-True ($recoveredEntries.Count -eq 9) "unlocked recovery run did not persist the discovered table"
Assert-True ($recoveryLog.Contains("updated:")) "unlocked recovery run did not report a persisted update"

Write-Host "[11/12] WaveTrace=false is decided inside ReflectGen"
$configAfter = Get-Content -LiteralPath $singleConfig -Raw
$disabledConfigText = $configAfter.Replace('"WaveTrace": true', '"WaveTrace": false')
Assert-True ($disabledConfigText -ne $configAfter) "serialized config did not contain the WaveTrace switch"
Write-Utf8NoBom $singleConfig $disabledConfigText
$disabledAllLog = Invoke-ReflectGen @(
    "--reflect-root-class", "Root",
    "--batch-dir", $batchInputDir, "--no-recursive",
    "--wavetrace-config", $singleConfig,
    "-o", $batchOut, "--aggregate-header", "project_reflect_auto.h",
    "--", "-x", "c++", "-std=c++14"
)
Assert-True ($disabledAllLog.Contains("skipped libclang/AST reflection")) "ReflectGen did not select its WaveTrace=false path"
$disabledAggregate = Get-Content -LiteralPath (Join-Path $batchOut "project_reflect_auto.h") -Raw
$disabledClosure = Get-Content -LiteralPath (Join-Path $batchOut "root_class_closure_reflect_auto.h") -Raw
Assert-True (-not $disabledClosure.Contains("ReflectAccess<")) "WaveTrace=false did not emit empty reflection"
Assert-GeneratedMember $disabledClosure "direct_ptr" $false
Assert-GeneratedMember $disabledClosure "template_ptr" $false
$disabledEntries = Read-ConfigEntries $singleConfig
Assert-True ($disabledEntries.Count -ge 9) "WaveTrace=false discarded the maintained pointer member table"

Write-Host "[12/12] Invalid pointer annotations fail with field-specific diagnostics"
$invalidInput = Join-Path $fixtureDir "invalid_input.fixture"
$invalidOut = Join-Path $BuildRoot "invalid\invalid_reflect.h"
$invalidConfig = Join-Path $BuildRoot "invalid\wavetrace_config.json"
$invalidLog = Invoke-ReflectGen @(
    $invalidInput, "-o", $invalidOut,
    "--header-include", "invalid_input.fixture",
    "--wavetrace-config", $invalidConfig,
    "--main-file-only", "--", "-x", "c++", "-std=c++14"
) 3
Assert-True ($invalidLog.Contains("WAVE_PTR/WAVE_PTR_ARRAY requires")) "non-pointer WAVE_PTR was not rejected"
Assert-True ($invalidLog.Contains("WAVE_PTR_ARRAY does not support std::weak_ptr")) "weak pointer array was not rejected"
Assert-True ($invalidLog.Contains("length must be an integer literal or member identifier")) "unstable array length expression was not rejected"

Write-Host "PASS: all ReflectGen pointer configuration cases passed"
