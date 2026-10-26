param(
    [Parameter(Mandatory)][int]$TargetProcessId,
    [Parameter(Mandatory)][string]$OutFile,
    [int]$IntervalMs = 500
)
$ErrorActionPreference = 'Stop'
$writer = [IO.StreamWriter]::new($OutFile, $false, [Text.UTF8Encoding]::new($false))
try {
    while ($null -ne (Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue)) {
        $rows = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
            Where-Object { $_.Name -like "pid_${TargetProcessId}_*" })
        $process = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
        if ($rows.Count -gt 0) {
            $record = [ordered]@{
                unix_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
                pid = $TargetProcessId
                dedicated_bytes = [long](($rows | Measure-Object DedicatedUsage -Sum).Sum)
                shared_bytes = [long](($rows | Measure-Object SharedUsage -Sum).Sum)
                committed_bytes = [long](($rows | Measure-Object TotalCommitted -Sum).Sum)
                working_set_bytes = if ($process) { $process.WorkingSet64 } else { $null }
                instances = @($rows | Select-Object Name,DedicatedUsage,SharedUsage,TotalCommitted)
            }
            $writer.WriteLine(($record | ConvertTo-Json -Compress -Depth 4))
            $writer.Flush()
        }
        Start-Sleep -Milliseconds $IntervalMs
    }
} finally {
    $writer.Dispose()
}
