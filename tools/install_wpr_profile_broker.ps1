[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskName = 'WaveTrace_WPR_Profiler'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This one-time installer must run elevated.'
}

$brokerScript = Join-Path $PSScriptRoot 'wpr_profile_broker.ps1'
if (-not (Test-Path -LiteralPath $brokerScript)) {
    throw "Profiler broker script not found: $brokerScript"
}

$userId = "$env:USERDOMAIN\$env:USERNAME"
$quotedBroker = '"' + $brokerScript.Replace('"', '""') + '"'
$action = New-ScheduledTaskAction `
    -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File $quotedBroker"
$taskPrincipal = New-ScheduledTaskPrincipal `
    -UserId $userId `
    -LogonType Interactive `
    -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Hours 12) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $taskName `
    -Action $action `
    -Principal $taskPrincipal `
    -Settings $settings `
    -Description 'Elevated WPR broker for WaveTrace CPU profiling.' `
    -Force | Out-Null

$brokerRoot = Join-Path $env:LOCALAPPDATA 'WaveTraceProfiler'
New-Item -ItemType Directory -Path $brokerRoot -Force | Out-Null
[pscustomobject]@{
    task = $taskName
    user = $userId
    broker = $brokerScript
    installed_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $brokerRoot 'installed.json') -Encoding UTF8

Write-Host "Installed scheduled task $taskName for $userId"
