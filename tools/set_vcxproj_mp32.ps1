param(
    [string]$Root = ".",
    [int]$Threads = 32,
    [string[]]$ExcludeDir = @(".git", "build", "build_vs", "out", "dist"),
    [switch]$Backup,
    [switch]$WhatIf
)

$ErrorActionPreference = "Stop"

$resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
$ExcludeDir = @($ExcludeDir | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$namespaceUri = "http://schemas.microsoft.com/developer/msbuild/2003"
$mpOption = "/MP$Threads"

function New-MsbuildElement([xml]$doc, [string]$name) {
    return $doc.CreateElement($name, $namespaceUri)
}

function Normalize-AdditionalOptions([string]$text, [string]$mpOption) {
    if ($null -eq $text) {
        $text = ""
    }

    $text = [regex]::Replace($text, "(?i)(^|\s)/MP\d*(?=\s|$)", " ")
    $text = [regex]::Replace($text, "\s+", " ").Trim()

    if ($text -match [regex]::Escape("%(AdditionalOptions)")) {
        $text = [regex]::Replace($text, [regex]::Escape("%(AdditionalOptions)"), "").Trim()
    }

    if ($text.Length -eq 0) {
        return "$mpOption %(AdditionalOptions)"
    }
    return "$mpOption $text %(AdditionalOptions)"
}

function Save-XmlUtf8NoBom([xml]$doc, [string]$path) {
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)
    $settings.Indent = $true
    $settings.NewLineChars = "`r`n"
    $settings.OmitXmlDeclaration = $false

    $writer = [System.Xml.XmlWriter]::Create($path, $settings)
    try {
        $doc.Save($writer)
    }
    finally {
        $writer.Close()
    }
}

$excludeRegex = $null
if ($ExcludeDir.Count -gt 0) {
    $escaped = $ExcludeDir | ForEach-Object { [regex]::Escape($_) }
    $excludeRegex = "\\($($escaped -join '|'))($|\\)"
}

$projects = Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Filter *.vcxproj
if ($excludeRegex) {
    $projects = $projects | Where-Object { $_.FullName -notmatch $excludeRegex }
}

$changed = 0
foreach ($project in $projects) {
    [xml]$doc = Get-Content -LiteralPath $project.FullName -Raw
    $ns = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
    $ns.AddNamespace("msb", $namespaceUri)

    $itemDefinitionGroups = @($doc.SelectNodes("//msb:ItemDefinitionGroup", $ns))
    if ($itemDefinitionGroups.Count -eq 0) {
        $itemDefinitionGroups = @($doc.Project.AppendChild((New-MsbuildElement $doc "ItemDefinitionGroup")))
    }

    $projectChanged = $false
    foreach ($group in $itemDefinitionGroups) {
        $clCompile = $group.SelectSingleNode("msb:ClCompile", $ns)
        if ($null -eq $clCompile) {
            $clCompile = $group.AppendChild((New-MsbuildElement $doc "ClCompile"))
            $projectChanged = $true
        }

        $mp = $clCompile.SelectSingleNode("msb:MultiProcessorCompilation", $ns)
        if ($null -eq $mp) {
            $mp = $clCompile.AppendChild((New-MsbuildElement $doc "MultiProcessorCompilation"))
            $projectChanged = $true
        }
        if ($mp.InnerText -ne "true") {
            $mp.InnerText = "true"
            $projectChanged = $true
        }

        $additional = $clCompile.SelectSingleNode("msb:AdditionalOptions", $ns)
        if ($null -eq $additional) {
            $additional = $clCompile.AppendChild((New-MsbuildElement $doc "AdditionalOptions"))
            $projectChanged = $true
        }

        $newOptions = Normalize-AdditionalOptions $additional.InnerText $mpOption
        if ($additional.InnerText -ne $newOptions) {
            $additional.InnerText = $newOptions
            $projectChanged = $true
        }
    }

    if ($projectChanged) {
        ++$changed
        if ($WhatIf) {
            Write-Host "would update: $($project.FullName)"
        }
        else {
            if ($Backup) {
                Copy-Item -LiteralPath $project.FullName -Destination ($project.FullName + ".bak") -Force
            }
            Save-XmlUtf8NoBom $doc $project.FullName
            Write-Host "updated: $($project.FullName)"
        }
    }
}

Write-Host "scanned=$($projects.Count) changed=$changed threads=$Threads root=$resolvedRoot"
