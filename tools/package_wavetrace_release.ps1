param(
    [string]$OutputDir = "",
    [string]$PackageName = ""
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if ($OutputDir -eq "") {
    $OutputDir = Join-Path $root "dist"
}
if ($PackageName -eq "") {
    $PackageName = "WaveTracer_release_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss")
}

$outRoot = New-Item -ItemType Directory -Force -Path $OutputDir
$stageParent = Join-Path $outRoot.FullName "_stage"
$packageTop = "WaveTracer"
$stageRoot = Join-Path $stageParent $packageTop
$zipPath = Join-Path $outRoot.FullName ($PackageName + ".zip")

if (Test-Path -LiteralPath $stageParent) {
    Remove-Item -LiteralPath $stageParent -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot "generated_reflect") | Out-Null

function Copy-ReleaseFile([string]$relativePath) {
    $src = Join-Path $root $relativePath
    if (!(Test-Path -LiteralPath $src)) {
        throw "Missing package input: $relativePath"
    }
    $dst = Join-Path $stageRoot $relativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

function Copy-ReleaseDir([string]$relativePath) {
    $src = Join-Path $root $relativePath
    if (!(Test-Path -LiteralPath $src)) {
        throw "Missing package input directory: $relativePath"
    }
    $dst = Join-Path $stageRoot $relativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
}

$files = @(
    "WaveTrace_使用说明.md",
    "WaveViewer_使用说明.md",
    "README_BUILD_VS.md",
    "README_WAVETAP_MANUAL.md",
    "README_WVZ4_V3_MONITOR_CHUNKS.md",
    "reflect_macro.h",
    "reflect_runtime.h",
    "wavetrace_config.h",
    "wavetrace_config.json",
    "wave_path_wvz4_recorder.h",
    "wave_runtime.h",
    "wave_tap.h",
    "wvz4_writer_typed.h",
    "cmake\run_reflectgen.cmake",
    "cmake\wavetrace_reflectgen.cmake",
    "cmake\wavetrace_writer_helper.cmake",
    "props\systemc_local.props",
    "props\wavetrace_app_common.props",
    "props\wavetrace_reflectgen_reference.props",
    "props\wvz4_writer_helper_reference.props",
    "props\zstd_embed.props",
    "tools\bin\wavetrace_reflectgen.exe",
    "tools\bin\wvz4_writer_monitor.exe",
    "tools\bin\libclang.dll",
    "tools\bin\LLVM-C.dll",
    "tools\bin\LTO.dll",
    "tools\bin\Remarks.dll",
    "tools\bin\libomp.dll",
    "tools\bin\libiomp5md.dll",
    "tools\package_wavetrace_release.ps1",
    "tools\collect_cmake_build_type_files.ps1",
    "tools\collect_cmake_build_type_files_oneclick.bat",
    "tools\set_vcxproj_mp32.ps1",
    "tools\set_vcxproj_mp32_oneclick.bat",
    "tools\lib\zstd_release.lib",
    "tools\lib\zstd_debug.lib",
    "third_party\zstd\include\zstd.h",
    "third_party\zstd\include\zstd_errors.h"
)

foreach ($file in $files) {
    Copy-ReleaseFile $file
}

Copy-ReleaseDir "tools\lib\clang"

$viewerFiles = @(
    "QtViewer\ActiveSignalItemWidget.cpp",
    "QtViewer\ActiveSignalItemWidget.h",
    "QtViewer\ActiveSignalListWidget.cpp",
    "QtViewer\ActiveSignalListWidget.h",
    "QtViewer\app.ico",
    "QtViewer\app.manifest",
    "QtViewer\app.rc",
    "QtViewer\icons\compare.png",
    "QtViewer\main.cpp",
    "QtViewer\MainWindow.cpp",
    "QtViewer\MainWindow.h",
    "QtViewer\props\zstd_embed.props",
    "QtViewer\QtLocal.props",
    "QtViewer\QtSingalViewer.qrc",
    "QtViewer\QtSingalViewer.ui",
    "QtViewer\QtViewer.sln",
    "QtViewer\QtViewer.vcxproj",
    "QtViewer\README_BUILD_VIEWER.md",
    "QtViewer\resources.qrc",
    "QtViewer\WaveCanvas.cpp",
    "QtViewer\WaveCanvas.h",
    "QtViewer\WaveParser4.cpp",
    "QtViewer\WaveParser4.h",
    "QtViewer\WaveTypes.h",
    "QtViewer\wave_path_wvz4_recorder.h",
    "QtViewer\wvz4_writer_typed.h"
)

foreach ($file in $viewerFiles) {
    Copy-ReleaseFile $file
}

$viewerRuntimeFiles = @(
    "QtViewer\build\x64\Release\WaveViewer.exe",
    "QtViewer\build\x64\Release\Qt5Core.dll",
    "QtViewer\build\x64\Release\Qt5Gui.dll",
    "QtViewer\build\x64\Release\Qt5Widgets.dll",
    "QtViewer\build\x64\Release\platforms\qwindows.dll"
)

foreach ($file in $viewerRuntimeFiles) {
    Copy-ReleaseFile $file
}

$qtRoot = "QtViewer\third_party\Qt\5.15.2\msvc2019_64"
$qtFiles = @(
    "bin\moc.exe",
    "bin\rcc.exe",
    "bin\uic.exe",
    "bin\Qt5Core.dll",
    "bin\Qt5Gui.dll",
    "bin\Qt5Widgets.dll",
    "lib\qtmain.lib",
    "lib\Qt5Core.lib",
    "lib\Qt5Gui.lib",
    "lib\Qt5Widgets.lib",
    "plugins\platforms\qwindows.dll"
)

foreach ($file in $qtFiles) {
    Copy-ReleaseFile (Join-Path $qtRoot $file)
}

Copy-ReleaseDir (Join-Path $qtRoot "include\QtCore")
Copy-ReleaseDir (Join-Path $qtRoot "include\QtGui")
Copy-ReleaseDir (Join-Path $qtRoot "include\QtWidgets")

Compress-Archive -Path (Join-Path $stageParent $packageTop) -DestinationPath $zipPath -Force
Remove-Item -LiteralPath $stageParent -Recurse -Force

$blocked = @(
    "\SystemC\",
    "/SystemC/",
    "systemc-",
    "smoke",
    "\tests\",
    "/tests/",
    "zstd-src",
    "ReflectGen.vcxproj",
    "wvz4_writer_monitor.vcxproj",
    "third_party\llvm"
)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($entry in $zip.Entries) {
        foreach ($needle in $blocked) {
            if ($entry.FullName -like "*$needle*") {
                throw "Blocked formal-package path found: $($entry.FullName)"
            }
        }
    }
}
finally {
    $zip.Dispose()
}

$bytes = (Get-Item -LiteralPath $zipPath).Length
Write-Host "package=$zipPath"
Write-Host "bytes=$bytes"
