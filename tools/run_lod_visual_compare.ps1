param(
    [string]$ViewerExe = "QtViewer\build\x64\Release\WaveViewer.exe",
    [string]$OutputRoot = "build_vs",
    [int]$Width = 1600,
    [int]$Height = 420
)

$ErrorActionPreference = "Stop"

$exePath = Resolve-Path $ViewerExe
$cases = @(
    @{ Name = "allchange_10m"; Path = "build_vs\business_10sig_allchange_10m.wvz4"; Signals = 6; Steps = 12; MaxDecodedSamples = 300000000 },
    @{ Name = "random_2048_200k"; Path = "build_vs\wvz4_lod_random_2048_200k.wvz4"; Signals = 8; Steps = 10; MaxDecodedSamples = 200000000 },
    @{ Name = "valid_ranges_v12"; Path = "build_vs\wvz4_lodz_valid_ranges_v12_smoke.wvz4"; Signals = 8; Steps = 10; MaxDecodedSamples = 200000000 },
    @{ Name = "wide_sparse_1m_32s"; Path = "build_vs\wvz4_lodz_wide_1mcycles_32signals_3upd_helper.wvz4"; Signals = 8; Steps = 10; MaxDecodedSamples = 200000000 },
    @{ Name = "stress_13gb_smoke"; Path = "build_vs\system_stress_current_1mcycles_1msignals_2048u_newlod.wvz4"; Signals = 4; Steps = 4; MaxDecodedSamples = 50000000 }
)

$summary = @()
foreach ($case in $cases) {
    if (!(Test-Path $case.Path)) {
        $summary += [pscustomobject]@{
            Case = $case.Name
            Exit = "missing"
            WallMs = ""
            WorstStep = ""
            ActiveJaccard = ""
            ActiveMiss = ""
            ActiveExtra = ""
            ExactDiff = ""
            Output = ""
        }
        continue
    }

    $outDir = Join-Path $OutputRoot ("lod_visual_compare_" + $case.Name)
    if (Test-Path $outDir) {
        Remove-Item -LiteralPath $outDir -Recurse -Force
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exePath -ArgumentList @(
        "--lod-visual-compare",
        $case.Path,
        $outDir,
        [string]$case.Signals,
        [string]$case.Steps,
        [string]$Width,
        [string]$Height,
        [string]$case.MaxDecodedSamples
    ) -Wait -PassThru -WindowStyle Hidden
    $sw.Stop()

    $csvPath = Join-Path $outDir "lod_visual_compare.csv"
    $metrics = @{}
    if (Test-Path $csvPath) {
        foreach ($line in Get-Content $csvPath) {
            $parts = $line -split ",", 2
            if ($parts.Count -eq 2) {
                $metrics[$parts[0]] = $parts[1]
            }
        }
    }

    $summary += [pscustomobject]@{
        Case = $case.Name
        Exit = $process.ExitCode
        WallMs = $sw.ElapsedMilliseconds
        WorstStep = $metrics["worst_step"]
        ActiveJaccard = $metrics["worst_active_jaccard"]
        ActiveMiss = $metrics["worst_active_miss_ratio"]
        ActiveExtra = $metrics["worst_active_extra_ratio"]
        ExactDiff = $metrics["worst_exact_diff_ratio"]
        Output = $outDir
    }
}

$summary | Format-Table -AutoSize
