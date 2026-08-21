$ErrorActionPreference = 'Stop'

$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$writerProject = Join-Path $PSScriptRoot 'wavefifo_layout_modes_writer.vcxproj'
$toolProject = Join-Path $root 'QtViewer\WaveFifoPerf.vcxproj'

& $msbuild $writerProject /m /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $msbuild $toolProject /m /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$writer = Join-Path $PSScriptRoot 'build\wavefifo_layout_modes_writer.exe'
$tool = Join-Path $root 'QtViewer\build\x64\Release\WaveFifoPerf.exe'
$output = Join-Path $PSScriptRoot 'build'

foreach ($mode in @('normal', 'flat')) {
    $wave = Join-Path $output "$mode.wvz4"
    $report = Join-Path $output "$mode.fifo.perf"
    & $writer $mode $wave
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $tool $wave --start-cycle 0 --end-cycle 100 --out $report --no-progress
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    $data = Get-Content -Raw (Join-Path $report 'data.json') | ConvertFrom-Json
    if ($data.summary.confirmed_resource_count -ne 3) { throw "${mode}: resource count" }
    if ($data.summary.confirmed_fifo_count -ne 2) { throw "${mode}: FIFO count" }
    if ($data.summary.confirmed_queue_count -ne 1) { throw "${mode}: Queue count" }
    if ($data.summary.decoded_signal_count -ne 6) { throw "${mode}: decoded count" }
    if ($data.summary.rejected_candidate_count -ne 2) { throw "${mode}: rejected count" }
    if ($data.summary.directory_signals_scanned -ne 46) { throw "${mode}: scanned count" }
    if ($data.summary.full_paths_built -ne 8) { throw "${mode}: full-path filter" }
    if (($data.resources.occupancy_kind -join ',') -ne 'm_count,m_numAvail,m_num_readable') {
        throw "${mode}: occupancy kinds"
    }
    if (($data.resources.resource_kind -join ',') -ne 'queue,fifo,fifo') {
        throw "${mode}: resource kinds"
    }
    if (($data.resources.full_rate_percent -join ',') -ne '60,50,30') {
        throw "${mode}: full rates"
    }
    if (($data.resources.path | Where-Object { $_ -match '\.(m_size|m_num_readable|m_numAvail|m_count)$' }).Count -ne 0) {
        throw "${mode}: child signal leaked into resource path"
    }
    if (($data | ConvertTo-Json -Depth 8).Contains('low_confidence')) {
        throw "${mode}: confidence leaked"
    }
    $occupancySorted = @($data.resources | Sort-Object occupancy_rate_percent -Descending)
    if (($occupancySorted.path | ForEach-Object { ($_ -split '\.')[-1] }) -join ',' -ne 'object_a,object_c,object_b') {
        throw "${mode}: occupancy-rate order"
    }
    $html = Get-Content -Raw (Join-Path $report 'index.html')
    if (-not $html.Contains('id="sortMode"') -or
        -not $html.Contains("mode==='occupancy'")) {
        throw "${mode}: sort selector"
    }
    if (-not $html.Contains('id="pathFilter"') -or
        -not $html.Contains("addEventListener('input',renderRows)") -or
        -not $html.Contains(".includes(query)")) {
        throw "${mode}: path search"
    }
    if ($html.Contains('<th>占用字段</th>') -or
        $html.Contains('<th>覆盖</th>')) {
        throw "${mode}: redundant columns"
    }
    if ($mode -eq 'flat' -and
        ($data.resources.representative_only | Where-Object { $_ -ne $true }).Count -ne 0) {
        throw 'flat: representative markers'
    }
    if ((Select-String -Path (Join-Path $report 'index.html') -Pattern 'const DATA=').Count -ne 1) {
        throw "${mode}: embedded JSON"
    }
}

Write-Output 'wavefifo_layout_modes_ok'
