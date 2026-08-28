param(
    [string]$OutputDir = "",
    [string]$PackageName = "",
    [switch]$SkipBuild,
    [switch]$IncludeDependencies,
    [switch]$ExcludeDependencies,
    [switch]$KeepStage
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $root "dist"
}
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $outputRoot).Path

if ($IncludeDependencies -and $ExcludeDependencies) {
    throw "Use either -IncludeDependencies or -ExcludeDependencies, not both."
}
# Dependency-free is the formal/default package. -ExcludeDependencies remains
# accepted for compatibility with older invocations of this script.
$withDependencies = $IncludeDependencies -and !$ExcludeDependencies

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $suffix = if ($withDependencies) { "with_dependencies" } else { "no_dependencies_no_tests" }
    $PackageName = "WaveTracer_source_exe_projects_{0}_{1}" -f $suffix, (Get-Date -Format "yyyyMMdd_HHmmss")
}
if ($PackageName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw "PackageName contains invalid filename characters: $PackageName"
}

function Test-PathInside([string]$Candidate, [string]$Parent) {
    $candidateFull = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $candidateFull.StartsWith(
        $parentFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)
}

function Find-MSBuild {
    $known = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($candidate in $known) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found -and (Test-Path -LiteralPath $found -PathType Leaf)) { return $found }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "MSBuild.exe was not found. Install Visual Studio C++ tools or use -SkipBuild."
}

if (!$SkipBuild) {
    $msbuild = Find-MSBuild
    $projects = @(
        "ReflectGen.vcxproj",
        "wvz4_writer_monitor.vcxproj",
        "QtViewer\QtViewer.vcxproj",
        "QtViewer\WavePerf.vcxproj",
        "QtViewer\WaveFifoPerf.vcxproj"
    )
    foreach ($project in $projects) {
        $projectPath = Join-Path $root $project
        if (!(Test-Path -LiteralPath $projectPath -PathType Leaf)) {
            throw "Missing build project: $project"
        }
        Write-Host "building=$project"
        & $msbuild $projectPath /m /p:Configuration=Release /p:Platform=x64 /v:minimal
        if ($LASTEXITCODE -ne 0) {
            throw "Release x64 build failed: $project"
        }
    }
}

$stageParent = Join-Path $outputRoot ("_stage_" + $PackageName)
$stageRoot = Join-Path $stageParent "WaveTracer"
$zipPath = Join-Path $outputRoot ($PackageName + ".zip")
if (!(Test-PathInside $stageParent $outputRoot) -or !(Test-PathInside $zipPath $outputRoot)) {
    throw "Refusing to stage outside the selected output directory."
}
if (Test-Path -LiteralPath $stageParent) {
    Remove-Item -LiteralPath $stageParent -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "generated_reflect") | Out-Null

function Copy-PackageFile([string]$RelativePath) {
    $source = Join-Path $root $RelativePath
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing package input: $RelativePath"
    }
    $destination = Join-Path $stageRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Copy-PackageDirectory([string]$RelativePath) {
    $source = Join-Path $root $RelativePath
    if (!(Test-Path -LiteralPath $source -PathType Container)) {
        throw "Missing package input directory: $RelativePath"
    }
    $destination = Join-Path $stageRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

$rootFiles = @(
    "CMakeLists.txt", "WaveTrace.sln", "WaveTrace_使用说明.md",
    "WaveViewer_使用说明.md", "README_BUILD_VS.md", "README_WAVETAP_MANUAL.md",
    "README_WVZ4_V3_MONITOR_CHUNKS.md", "reflect_macro.h", "reflect_runtime.h",
    "ReflectGen.cpp", "ReflectGen.vcxproj", "wavetrace_config.h",
    "wavetrace_config.json", "wave_path_wvz4_recorder.h", "wave_runtime.h",
    "wave_tap.h", "wvz4_writer_monitor_main.cpp", "wvz4_writer_monitor.vcxproj",
    "wvz4_writer_typed.h", "cmake\run_cmodel_reggen.cmake",
    "cmake\run_reflectgen.cmake", "cmake\wavetrace_reflectgen.cmake",
    "cmake\wavetrace_writer_helper.cmake", "docs\WaveTrace_使用手册_20260711.md",
    "props\llvm_local.props", "props\systemc_local.props",
    "props\wavetrace_app_common.props", "props\wavetrace_reflectgen_reference.props",
    "props\wvz4_writer_helper_reference.props", "props\zstd_embed.props",
    "tools\bin\wavetrace_reflectgen.exe", "tools\bin\wvz4_writer_monitor.exe",
    "tools\package_wavetrace_release.ps1", "tools\collect_cmake_build_type_files.ps1",
    "tools\collect_cmake_build_type_files_oneclick.bat", "tools\set_vcxproj_mp32.ps1",
    "tools\set_vcxproj_mp32_oneclick.bat"
)

$viewerFiles = @(
    "QtViewer\ActiveSignalItemWidget.cpp", "QtViewer\ActiveSignalItemWidget.h",
    "QtViewer\ActiveSignalListWidget.cpp", "QtViewer\ActiveSignalListWidget.h",
    "QtViewer\AgentRpcServer.cpp", "QtViewer\AgentRpcServer.h", "QtViewer\app.ico",
    "QtViewer\app.manifest", "QtViewer\app.rc", "QtViewer\icons\compare.png",
    "QtViewer\main.cpp", "QtViewer\MainWindow.cpp", "QtViewer\MainWindow.h",
    "QtViewer\props\zstd_embed.props", "QtViewer\QtLocal.props",
    "QtViewer\QtSingalViewer.qrc", "QtViewer\QtSingalViewer.ui",
    "QtViewer\QtViewer.sln", "QtViewer\QtViewer.vcxproj",
    "QtViewer\README_BUILD_VIEWER.md", "QtViewer\resources.qrc",
    "QtViewer\wave_path_wvz4_recorder.h", "QtViewer\WaveBlockCacheLoader.cpp",
    "QtViewer\WaveBlockCacheLoader.h", "QtViewer\WaveCanvas.cpp",
    "QtViewer\WaveCanvas.h", "QtViewer\WaveFifoPerf.cpp",
    "QtViewer\WaveFifoPerf.vcxproj", "QtViewer\WaveFifoPerfOutput.cpp",
    "QtViewer\WaveFifoPerfOutput.h", "QtViewer\WaveFifoPressure.cpp",
    "QtViewer\WaveFifoPressure.h", "QtViewer\WaveParser4.cpp",
    "QtViewer\WaveParser4.h", "QtViewer\WavePerf.cpp", "QtViewer\WavePerf.vcxproj",
    "QtViewer\WavePerfArchitecture.cpp", "QtViewer\WavePerfArchitecture.h",
    "QtViewer\WavePerfBandwidth.cpp", "QtViewer\WavePerfBandwidth.h",
    "QtViewer\WavePerfCBCtrl.cpp", "QtViewer\WavePerfCBCtrl.h",
    "QtViewer\WavePerfDiagnosis.cpp", "QtViewer\WavePerfDiagnosis.h",
    "QtViewer\WavePerfInstructionFeatures.def", "QtViewer\WavePerfOutput.cpp",
    "QtViewer\WavePerfOutput.h", "QtViewer\WavePerfScheduler.cpp",
    "QtViewer\WavePerfScheduler.h", "QtViewer\WaveTypes.h",
    "QtViewer\wvz4_writer_typed.h", "QtViewer\docs\WaveFifoPerf.md",
    "QtViewer\build\x64\Release\WaveViewer.exe",
    "QtViewer\build\x64\Release\WavePerf.exe",
    "QtViewer\build\x64\Release\WaveFifoPerf.exe"
)

foreach ($file in $rootFiles + $viewerFiles) { Copy-PackageFile $file }

if ($withDependencies) {
    $dependencyFiles = @(
        "tools\bin\libclang.dll", "tools\bin\LLVM-C.dll", "tools\bin\LTO.dll",
        "tools\bin\Remarks.dll", "tools\bin\libomp.dll", "tools\bin\libiomp5md.dll",
        "tools\lib\zstd_release.lib", "tools\lib\zstd_debug.lib",
        "third_party\zstd\include\zstd.h", "third_party\zstd\include\zstd_errors.h",
        "QtViewer\build\x64\Release\Qt5Core.dll",
        "QtViewer\build\x64\Release\Qt5Gui.dll",
        "QtViewer\build\x64\Release\Qt5Widgets.dll",
        "QtViewer\build\x64\Release\platforms\qwindows.dll"
    )
    foreach ($file in $dependencyFiles) { Copy-PackageFile $file }
    Copy-PackageDirectory "tools\lib\clang"
}

Compress-Archive -LiteralPath $stageRoot -DestinationPath $zipPath -CompressionLevel Optimal

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $entries = @($zip.Entries | Where-Object { ![string]::IsNullOrEmpty($_.Name) })
    $blocked = @(
        "/tests/", "smoke", "/tmp/", "/build_vs/", "/generated_reflect/",
        ".obj", ".pdb", ".ilk", ".log", ".wvz4", ".diagsession"
    )
    if (!$withDependencies) {
        $blocked += @(
            "/third_party/", "/tools/lib/", "libclang.dll", "LLVM-C.dll",
            "LTO.dll", "Remarks.dll", "libomp.dll", "libiomp5md.dll",
            "Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll", "qwindows.dll"
        )
    }
    foreach ($entry in $entries) {
        $entryPath = "/" + $entry.FullName.Replace('\', '/')
        foreach ($needle in $blocked) {
            if ($entryPath.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "Blocked package entry found: $($entry.FullName)"
            }
        }
    }

    $required = @(
        "WaveTracer/CMakeLists.txt", "WaveTracer/WaveTrace.sln",
        "WaveTracer/ReflectGen.cpp", "WaveTracer/ReflectGen.vcxproj",
        "WaveTracer/QtViewer/MainWindow.cpp", "WaveTracer/QtViewer/WaveCanvas.cpp",
        "WaveTracer/QtViewer/WavePerf.cpp", "WaveTracer/QtViewer/WaveFifoPerf.cpp",
        "WaveTracer/QtViewer/build/x64/Release/WaveViewer.exe",
        "WaveTracer/QtViewer/build/x64/Release/WavePerf.exe",
        "WaveTracer/QtViewer/build/x64/Release/WaveFifoPerf.exe",
        "WaveTracer/tools/bin/wavetrace_reflectgen.exe"
    )
    $entrySet = @{}
    foreach ($entry in $entries) { $entrySet[$entry.FullName.Replace('\', '/')] = $true }
    foreach ($path in $required) {
        if (!$entrySet.ContainsKey($path)) { throw "Required package entry missing: $path" }
    }
    $fileCount = $entries.Count
    $uncompressedBytes = ($entries | Measure-Object Length -Sum).Sum
}
finally {
    $zip.Dispose()
}

if (!$KeepStage) {
    if (!(Test-PathInside $stageParent $outputRoot)) {
        throw "Refusing to remove staging directory outside output root."
    }
    Remove-Item -LiteralPath $stageParent -Recurse -Force
}

$zipBytes = (Get-Item -LiteralPath $zipPath).Length
$sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host "package=$zipPath"
Write-Host "files=$fileCount"
Write-Host "uncompressed_bytes=$uncompressedBytes"
Write-Host "zip_bytes=$zipBytes"
Write-Host "sha256=$sha256"
