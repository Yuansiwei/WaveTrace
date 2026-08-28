[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $WprArguments
)

$ErrorActionPreference = 'Stop'

$taskName = 'WaveTrace_WPR_Profiler'
$brokerRoot = Join-Path $env:LOCALAPPDATA 'WaveTraceProfiler'
$requestPath = Join-Path $brokerRoot 'request.json'

function Test-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-WprBroker {
    param([string[]] $Arguments)

    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $task) { return $false }

    New-Item -ItemType Directory -Path $brokerRoot -Force | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Get-ScheduledTask -TaskName $taskName).State -eq 'Running') {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "The WPR profiler broker is still busy after 30 seconds"
        }
        Start-Sleep -Milliseconds 100
    }

    $normalized = @($Arguments)
    for ($i = 0; $i -lt $normalized.Count; ++$i) {
        if ($normalized[$i] -ieq '-stop' -and $i + 1 -lt $normalized.Count) {
            $normalized[$i + 1] = [IO.Path]::GetFullPath($normalized[$i + 1])
            $parent = Split-Path -Parent $normalized[$i + 1]
            if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            break
        }
    }

    $id = [Guid]::NewGuid().ToString('N')
    $responsePath = Join-Path $brokerRoot ("response_$id.json")
    $temporaryRequest = "$requestPath.$id.tmp"
    [pscustomobject]@{
        id = $id
        arguments = $normalized
        response_path = $responsePath
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporaryRequest -Encoding UTF8
    Move-Item -LiteralPath $temporaryRequest -Destination $requestPath -Force

    Start-ScheduledTask -TaskName $taskName
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while (-not (Test-Path -LiteralPath $responsePath)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out waiting for the elevated WPR profiler broker"
        }
        Start-Sleep -Milliseconds 100
    }

    $response = Get-Content -Raw -LiteralPath $responsePath | ConvertFrom-Json
    Remove-Item -LiteralPath $responsePath -Force -ErrorAction SilentlyContinue
    if ($response.output) { [Console]::Out.Write([string]$response.output) }
    exit [int]$response.exit_code
}

if (-not (Test-IsElevated)) {
    Invoke-WprBroker -Arguments $WprArguments
}

if (-not ('WaveTrace.ProfilePrivilege' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace WaveTrace {
    public static class ProfilePrivilege {
        private const UInt32 TOKEN_QUERY = 0x0008;
        private const UInt32 TOKEN_ADJUST_PRIVILEGES = 0x0020;
        private const UInt32 SE_PRIVILEGE_ENABLED = 0x00000002;
        private const Int32 ERROR_NOT_ALL_ASSIGNED = 1300;

        [StructLayout(LayoutKind.Sequential)]
        private struct LUID {
            public UInt32 LowPart;
            public Int32 HighPart;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct TOKEN_PRIVILEGES {
            public UInt32 PrivilegeCount;
            public LUID Luid;
            public UInt32 Attributes;
        }

        [DllImport("kernel32.dll")]
        private static extern IntPtr GetCurrentProcess();

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool OpenProcessToken(
            IntPtr ProcessHandle, UInt32 DesiredAccess, out IntPtr TokenHandle);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool LookupPrivilegeValue(
            string SystemName, string Name, out LUID Luid);

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool AdjustTokenPrivileges(
            IntPtr TokenHandle,
            bool DisableAllPrivileges,
            ref TOKEN_PRIVILEGES NewState,
            UInt32 BufferLength,
            IntPtr PreviousState,
            IntPtr ReturnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr Handle);

        public static void EnableSystemProfile() {
            IntPtr token;
            if (!OpenProcessToken(GetCurrentProcess(),
                                  TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                                  out token)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "OpenProcessToken failed");
            }
            try {
                LUID luid;
                if (!LookupPrivilegeValue(null, "SeSystemProfilePrivilege", out luid)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "LookupPrivilegeValue failed");
                }
                TOKEN_PRIVILEGES privileges = new TOKEN_PRIVILEGES();
                privileges.PrivilegeCount = 1;
                privileges.Luid = luid;
                privileges.Attributes = SE_PRIVILEGE_ENABLED;
                if (!AdjustTokenPrivileges(token, false, ref privileges,
                                           0, IntPtr.Zero, IntPtr.Zero)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "AdjustTokenPrivileges failed");
                }
                int error = Marshal.GetLastWin32Error();
                if (error == ERROR_NOT_ALL_ASSIGNED) {
                    throw new Win32Exception(error,
                        "The current logon token does not hold SeSystemProfilePrivilege");
                }
            }
            finally {
                CloseHandle(token);
            }
        }
    }
}
'@
}

[WaveTrace.ProfilePrivilege]::EnableSystemProfile()

$wpr = Join-Path $env:WINDIR 'System32\wpr.exe'
if (-not (Test-Path -LiteralPath $wpr)) {
    throw "Windows Performance Recorder was not found: $wpr"
}

& $wpr @WprArguments
exit $LASTEXITCODE
