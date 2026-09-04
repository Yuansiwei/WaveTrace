param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found: $vswhere"
}

$vs = (& $vswhere -latest -products * -property installationPath).Trim()
if (-not $vs) { throw "Visual Studio installation was not found" }

$csc = Join-Path $vs "MSBuild\Current\Bin\Roslyn\csc.exe"
$private = Join-Path $vs "Common7\IDE\PrivateAssemblies"
$traceEvent = Join-Path $private "Microsoft.Diagnostics.Tracing.TraceEvent.dll"
$frameworkRoot = "C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework"
$netstandard = Get-ChildItem -LiteralPath $frameworkRoot -Recurse -Filter netstandard.dll |
    Where-Object { $_.FullName -match '\\Facades\\netstandard\.dll$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not (Test-Path -LiteralPath $csc)) { throw "C# compiler was not found: $csc" }
if (-not (Test-Path -LiteralPath $traceEvent)) { throw "TraceEvent was not found: $traceEvent" }
if (-not (Test-Path -LiteralPath $netstandard)) { throw "netstandard reference facade was not found: $netstandard" }

function Find-AssemblyVersion([string]$name, [version]$version) {
    $match = Get-ChildItem -LiteralPath $vs -Recurse -Filter $name -ErrorAction SilentlyContinue |
        Where-Object {
            try { [Reflection.AssemblyName]::GetAssemblyName($_.FullName).Version -eq $version }
            catch { $false }
        } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $match) { throw "$name version $version was not found under $vs" }
    return $match
}

$unsafe = Find-AssemblyVersion "System.Runtime.CompilerServices.Unsafe.dll" ([version]"4.0.4.1")

$out = Join-Path $root "build_vs\diagsession_top\$Configuration"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$references = @(
    $traceEvent,
    (Join-Path $private "Microsoft.Diagnostics.FastSerialization.dll"),
    (Join-Path $private "Dia2Lib.dll"),
    (Join-Path $private "System.Collections.Immutable.dll"),
    (Join-Path $private "System.Reflection.Metadata.dll"),
    $unsafe,
    (Join-Path $private "System.Runtime.InteropServices.RuntimeInformation.dll"),
    (Join-Path $private "System.Memory.dll"),
    (Join-Path $private "System.Buffers.dll"),
    (Join-Path $private "System.Numerics.Vectors.dll"),
    (Join-Path $private "System.Threading.Tasks.Extensions.dll"),
    (Join-Path $private "TraceReloggerLib.dll"),
    (Join-Path $private "OSExtensions.dll")
) | Where-Object { Test-Path -LiteralPath $_ }

$args = @(
    "/nologo",
    "/target:exe",
    "/platform:x64",
    "/langversion:7.3",
    "/optimize+",
    "/debug:pdbonly",
    "/out:$out\DiagSessionTop.exe",
    "/reference:System.IO.Compression.dll",
    "/reference:System.IO.Compression.FileSystem.dll",
    "/reference:$netstandard"
)
foreach ($reference in $references) { $args += "/reference:$reference" }
$args += (Join-Path $PSScriptRoot "DiagSessionTop.cs")

& $csc @args
if ($LASTEXITCODE -ne 0) { throw "csc failed with exit code $LASTEXITCODE" }

foreach ($reference in $references) {
    Copy-Item -LiteralPath $reference -Destination $out -Force
}

$diaNative = Join-Path $vs "DIA SDK\bin\amd64\msdia140.dll"
if (-not (Test-Path -LiteralPath $diaNative)) {
    throw "64-bit DIA runtime was not found: $diaNative"
}
$diaOutputs = @(
    (Join-Path $out "native\amd64"),
    (Join-Path $root "build_vs\diagsession_top\native\amd64")
)
foreach ($diaOut in $diaOutputs) {
    New-Item -ItemType Directory -Force -Path $diaOut | Out-Null
    Copy-Item -LiteralPath $diaNative -Destination $diaOut -Force
}

Write-Host "Built: $out\DiagSessionTop.exe"
