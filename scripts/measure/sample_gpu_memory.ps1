param(
    [Parameter(Mandatory=$true)][int]$TargetProcessId,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
while (Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue) {
    $instances = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
        Where-Object { $_.Name -like "pid_${TargetProcessId}_*" })
    if ($instances.Count -gt 0) {
        [pscustomobject]@{
            unix_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            dedicated_bytes = [uint64](($instances | Measure-Object DedicatedUsage -Sum).Sum)
            shared_bytes = [uint64](($instances | Measure-Object SharedUsage -Sum).Sum)
            instances = $instances.Count
        } | ConvertTo-Json -Compress | Add-Content -LiteralPath $OutputPath -Encoding utf8
    }
    Start-Sleep -Milliseconds 250
}
