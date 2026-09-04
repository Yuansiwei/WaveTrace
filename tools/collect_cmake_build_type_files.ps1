param(
    [string]$Root = "",
    [string]$Output = "",
    [switch]$IncludeGenerated,
    [switch]$ExcludeThirdParty,
    [switch]$NoVsProjects,
    [switch]$NoVsGenerated,
    [switch]$OpenOutput
)

$ErrorActionPreference = "Stop"
$collectorScriptPath = $PSCommandPath

function Resolve-Root([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) {
        return (Get-Location).Path
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-RelativePath([string]$fullName, [string]$rootPath) {
    if ($fullName.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $fullName.Substring($rootPath.Length).TrimStart('\', '/')
    }
    return $fullName
}

function Is-Generated-Part([string]$part) {
    if ($part -match '^(build|build_vs|out|x64|Debug|Release|RelWithDebInfo|MinSizeRel)$') {
        return $true
    }
    if ($part -like '*_build' -or $part -like 'build-*') {
        return $true
    }
    return $false
}

function Is-ThirdParty-Part([string]$part) {
    return ($part -eq "third_party" -or
            $part -eq "3rdparty" -or
            $part -eq "external" -or
            $part -eq "extern")
}

function Should-Skip-Dir([string]$fullName, [string]$rootPath, [bool]$includeGeneratedForThisScan) {
    $relative = Get-RelativePath $fullName $rootPath
    if ([string]::IsNullOrWhiteSpace($relative)) { return $false }

    $parts = $relative -split '[\\/]'
    foreach ($part in $parts) {
        if ($part -eq ".git" -or $part -eq ".vs" -or $part -eq ".vscode" -or $part -eq "dist") {
            return $true
        }
        if (-not $includeGeneratedForThisScan -and (Is-Generated-Part $part)) {
            return $true
        }
        if ($ExcludeThirdParty -and (Is-ThirdParty-Part $part)) {
            return $true
        }
    }
    return $false
}

function Looks-Like-Text-Cmake-File([System.IO.FileInfo]$file) {
    if ($collectorScriptPath -and
        $file.FullName.Equals($collectorScriptPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }
    if ($file.Name -like "collect_cmake_build_type_files*") {
        return $false
    }

    $name = $file.Name
    $ext = $file.Extension.ToLowerInvariant()
    if ($name -ieq "CMakeLists.txt") { return $true }
    if ($ext -in @(".cmake", ".txt", ".bat", ".cmd", ".ps1", ".sh", ".md")) { return $true }
    return $false
}

function Looks-Like-Vs-Project-File([System.IO.FileInfo]$file) {
    $name = $file.Name.ToLowerInvariant()
    $ext = $file.Extension.ToLowerInvariant()
    if ($ext -in @(".sln", ".vcxproj", ".props", ".targets")) { return $true }
    if ($name -like "*.vcxproj.user") { return $true }
    return $false
}

function Append-Line([System.IO.StreamWriter]$writer, [string]$text = "") {
    $writer.WriteLine($text)
}

function Walk-Files([string]$rootPath, [bool]$includeGeneratedForThisScan, [scriptblock]$predicate) {
    $result = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    $dirs = New-Object System.Collections.Generic.Queue[System.IO.DirectoryInfo]
    $dirs.Enqueue((Get-Item -LiteralPath $rootPath))

    while ($dirs.Count -gt 0) {
        $dir = $dirs.Dequeue()
        foreach ($childDir in Get-ChildItem -LiteralPath $dir.FullName -Directory -Force -ErrorAction SilentlyContinue) {
            if (-not (Should-Skip-Dir $childDir.FullName $rootPath $includeGeneratedForThisScan)) {
                $dirs.Enqueue($childDir)
            }
        }
        foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -File -Force -ErrorAction SilentlyContinue) {
            if (& $predicate $file) {
                $result.Add($file)
            }
        }
    }

    return $result
}

function Is-Under-Root([string]$fullName, [string]$rootPath) {
    return $fullName.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-Local-Dependency([string]$baseDir, [string]$rawPath, [string]$rootPath) {
    if ([string]::IsNullOrWhiteSpace($rawPath)) { return $null }
    if ($rawPath -match '\$\(') { return $null }
    if ($rawPath -match '^\$') { return $null }

    $expanded = [Environment]::ExpandEnvironmentVariables($rawPath)
    $candidate = $expanded
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $baseDir $candidate
    }

    try {
        $resolved = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path
    }
    catch {
        return $null
    }

    if (-not (Is-Under-Root $resolved $rootPath)) { return $null }
    return $resolved
}

function Get-Vs-Referenced-Paths([System.IO.FileInfo]$file, [string]$rootPath) {
    $refs = New-Object System.Collections.Generic.List[string]
    $baseDir = $file.DirectoryName
    $ext = $file.Extension.ToLowerInvariant()

    if ($ext -eq ".sln") {
        try {
            $lines = Get-Content -LiteralPath $file.FullName -ErrorAction Stop
            foreach ($line in $lines) {
                if ($line -match 'Project\("[^"]+"\)\s*=\s*"[^"]+",\s*"([^"]+\.(?:vcxproj|vcproj))"') {
                    $resolved = Resolve-Local-Dependency $baseDir $matches[1] $rootPath
                    if ($resolved) { $refs.Add($resolved) }
                }
            }
        }
        catch {
        }
        return $refs
    }

    try {
        $lines = Get-Content -LiteralPath $file.FullName -ErrorAction Stop
        foreach ($line in $lines) {
            if ($line -match '<Import\s+Project="([^"]+)"') {
                $resolved = Resolve-Local-Dependency $baseDir $matches[1] $rootPath
                if ($resolved) { $refs.Add($resolved) }
            }
            if ($line -match '<ProjectReference\s+Include="([^"]+)"') {
                $resolved = Resolve-Local-Dependency $baseDir $matches[1] $rootPath
                if ($resolved) { $refs.Add($resolved) }
            }
            if ($line -match '<ImportGroup[^>]*Label="PropertySheets"') {
                # The imports themselves are handled by the generic Import parser.
                continue
            }
        }
    }
    catch {
    }

    return $refs
}

function Add-Vs-File([string]$fullName,
                     [string]$rootPath,
                     [hashtable]$map,
                     [System.Collections.Generic.Queue[System.IO.FileInfo]]$queue) {
    try {
        $item = Get-Item -LiteralPath $fullName -ErrorAction Stop
    }
    catch {
        return
    }
    if ($item.PSIsContainer) { return }
    if (-not (Looks-Like-Vs-Project-File $item)) { return }
    if (-not (Is-Under-Root $item.FullName $rootPath)) { return }

    $key = $item.FullName.ToLowerInvariant()
    if (-not $map.ContainsKey($key)) {
        $map[$key] = $item
        $queue.Enqueue($item)
    }
}

$rootPath = Resolve-Root $Root
if ([string]::IsNullOrWhiteSpace($Output)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $Output = Join-Path $rootPath ("cmake_vs_build_sources_{0}.txt" -f $stamp)
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
Write-Host "WaveTrace CMake/VS collector"
Write-Host ("root={0}" -f $rootPath)
Write-Host ("output={0}" -f $outputPath)

$patterns = @(
    "CMAKE_BUILD_TYPE",
    "CMAKE_CONFIGURATION_TYPES",
    "CMAKE_CFG_INTDIR",
    "BUILD_TYPE"
)

Write-Host "scan cmake/script files..."
$cmakeFiles = Walk-Files $rootPath ([bool]$IncludeGenerated) { param($f) Looks-Like-Text-Cmake-File $f }
$cmakeMatches = New-Object System.Collections.Generic.List[object]
foreach ($file in $cmakeFiles) {
    $relativePath = Get-RelativePath $file.FullName $rootPath
    try {
        $hitLines = Select-String -LiteralPath $file.FullName -Pattern $patterns -SimpleMatch -ErrorAction Stop
    }
    catch {
        continue
    }
    if ($hitLines) {
        $cmakeMatches.Add([pscustomobject]@{
            FullName = $file.FullName
            RelativePath = $relativePath
            Hits = @($hitLines)
        })
    }
}

$vsMatches = New-Object System.Collections.Generic.List[object]
if (-not $NoVsProjects) {
    $includeGeneratedVs = -not [bool]$NoVsGenerated
    Write-Host "scan vs project files..."
    $vsFiles = Walk-Files $rootPath $includeGeneratedVs { param($f) Looks-Like-Vs-Project-File $f }
    $vsMap = @{}
    $vsQueue = New-Object System.Collections.Generic.Queue[System.IO.FileInfo]

    foreach ($file in $vsFiles) {
        Add-Vs-File $file.FullName $rootPath $vsMap $vsQueue
    }

    while ($vsQueue.Count -gt 0) {
        $file = $vsQueue.Dequeue()
        foreach ($ref in Get-Vs-Referenced-Paths $file $rootPath) {
            Add-Vs-File $ref $rootPath $vsMap $vsQueue
        }
    }

    Write-Host "write vs project content..."
    foreach ($file in ($vsMap.Values | Sort-Object FullName)) {
        $relativePath = Get-RelativePath $file.FullName $rootPath
        $summaryHits = @()
        try {
            $summaryHits = Select-String -LiteralPath $file.FullName -Pattern @(
                'Project\("',
                '<ProjectConfiguration',
                '<Configuration>',
                '<Platform>',
                '<Import\s+Project=',
                '<ProjectReference\s+Include=',
                '<OutDir>',
                '<IntDir>',
                '<RuntimeLibrary>',
                '<AdditionalOptions>',
                '<PreprocessorDefinitions>',
                '<AdditionalIncludeDirectories>',
                'CMAKE_BUILD_TYPE',
                'CMAKE_CONFIGURATION_TYPES'
            ) -ErrorAction Stop
        }
        catch {
        }

        $vsMatches.Add([pscustomobject]@{
            FullName = $file.FullName
            RelativePath = $relativePath
            Hits = @($summaryHits)
        })
    }
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
Write-Host "write report..."
$writer = New-Object System.IO.StreamWriter($outputPath, $false, $utf8NoBom)
try {
    Append-Line $writer "WaveTrace CMake/VS build source collector"
    Append-Line $writer ("root: {0}" -f $rootPath)
    Append-Line $writer ("generated_at: {0:yyyy-MM-dd HH:mm:ss}" -f (Get-Date))
    Append-Line $writer ("include_generated_cmake_scan: {0}" -f ([bool]$IncludeGenerated))
    Append-Line $writer ("include_generated_vs_scan: {0}" -f (-not [bool]$NoVsGenerated))
    Append-Line $writer ("exclude_third_party: {0}" -f ([bool]$ExcludeThirdParty))
    Append-Line $writer ("cmake_matched_files: {0}" -f $cmakeMatches.Count)
    Append-Line $writer ("vs_project_files: {0}" -f $vsMatches.Count)
    Append-Line $writer ""

    Append-Line $writer "===== CMAKE BUILD-TYPE SUMMARY ====="
    foreach ($item in $cmakeMatches) {
        Append-Line $writer ("[{0}]" -f $item.RelativePath)
        foreach ($hit in $item.Hits) {
            Append-Line $writer ("  line {0}: {1}" -f $hit.LineNumber, $hit.Line.TrimEnd())
        }
    }

    Append-Line $writer ""
    Append-Line $writer "===== VS PROJECT SUMMARY ====="
    foreach ($item in $vsMatches) {
        Append-Line $writer ("[{0}]" -f $item.RelativePath)
        foreach ($hit in $item.Hits) {
            Append-Line $writer ("  line {0}: {1}" -f $hit.LineNumber, $hit.Line.TrimEnd())
        }
    }

    Append-Line $writer ""
    Append-Line $writer "===== CMAKE FULL FILE CONTENTS ====="
    foreach ($item in $cmakeMatches) {
        Append-Line $writer ""
        Append-Line $writer ("========== BEGIN CMAKE FILE: {0} ==========" -f $item.RelativePath)
        Append-Line $writer ("absolute_path: {0}" -f $item.FullName)
        Append-Line $writer "---------- CONTENT ----------"
        try {
            foreach ($line in Get-Content -LiteralPath $item.FullName -ErrorAction Stop) {
                Append-Line $writer $line
            }
        }
        catch {
            Append-Line $writer ("<failed to read file: {0}>" -f $_.Exception.Message)
        }
        Append-Line $writer ("========== END CMAKE FILE: {0} ==========" -f $item.RelativePath)
    }

    Append-Line $writer ""
    Append-Line $writer "===== VS PROJECT FULL FILE CONTENTS ====="
    foreach ($item in $vsMatches) {
        Append-Line $writer ""
        Append-Line $writer ("========== BEGIN VS FILE: {0} ==========" -f $item.RelativePath)
        Append-Line $writer ("absolute_path: {0}" -f $item.FullName)
        Append-Line $writer "---------- CONTENT ----------"
        try {
            foreach ($line in Get-Content -LiteralPath $item.FullName -ErrorAction Stop) {
                Append-Line $writer $line
            }
        }
        catch {
            Append-Line $writer ("<failed to read file: {0}>" -f $_.Exception.Message)
        }
        Append-Line $writer ("========== END VS FILE: {0} ==========" -f $item.RelativePath)
    }
}
finally {
    $writer.Dispose()
}

Write-Host ("root={0}" -f $rootPath)
Write-Host ("cmake_matched_files={0}" -f $cmakeMatches.Count)
Write-Host ("vs_project_files={0}" -f $vsMatches.Count)
Write-Host ("output={0}" -f $outputPath)
if ($OpenOutput) {
    Write-Host "open output..."
    Start-Process notepad.exe -ArgumentList "`"$outputPath`""
}
