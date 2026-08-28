$ErrorActionPreference = 'Stop'

$brokerRoot = Join-Path $env:LOCALAPPDATA 'WaveTraceProfiler'
$requestPath = Join-Path $brokerRoot 'request.json'
$wpr = Join-Path $env:WINDIR 'System32\wpr.exe'

if (-not (Test-Path -LiteralPath $requestPath)) {
    exit 2
}

$request = Get-Content -Raw -LiteralPath $requestPath | ConvertFrom-Json
Remove-Item -LiteralPath $requestPath -Force
$responsePath = [string]$request.response_path
$temporaryResponse = "$responsePath.tmp"
$exitCode = 1
$output = ''

try {
    if (-not (Test-Path -LiteralPath $wpr)) {
        throw "Windows Performance Recorder was not found: $wpr"
    }
    $arguments = @($request.arguments | ForEach-Object { [string]$_ })
    $output = (& $wpr @arguments 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
}
catch {
    $output = ($_ | Out-String)
    $exitCode = 1
}

[pscustomobject]@{
    id = [string]$request.id
    exit_code = [int]$exitCode
    output = [string]$output
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $temporaryResponse -Encoding UTF8
Move-Item -LiteralPath $temporaryResponse -Destination $responsePath -Force
exit $exitCode
