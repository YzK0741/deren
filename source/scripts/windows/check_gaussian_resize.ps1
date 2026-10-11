# Manual GPU regression: resize a hidden Gaussian viewer, capture its output and require clean sync validation.
# Config must name a PLY cloud and enable validation_layers. Inspect the saved image as well as the log.
param(
    [Parameter(Mandatory = $true)][string]$Config,
    [string]$BuildDir = 'build-release-dyn-clang64',
    [string]$WorkDir = '',
    [ValidateRange(320, 3840)][int]$Width = 960,
    [ValidateRange(240, 2160)][int]$Height = 640,
    [switch]$EarlyCapture,
    [switch]$RapidResize
)
$ErrorActionPreference = 'Stop'
if ($EarlyCapture -and $RapidResize) { throw 'Choose either EarlyCapture or RapidResize' }
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$configPath = (Resolve-Path -LiteralPath $Config).Path
$executable = Join-Path $buildPath 'deren.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "No renderer at $executable" }
if (-not $WorkDir) { $WorkDir = Join-Path $buildPath 'gaussian-resize-check' }
$workPath = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$probeConfig = Join-Path $workPath 'resize.toml'
if ($probeConfig -eq $configPath) { throw 'The input config must differ from the generated resize.toml' }
$configText = Get-Content -LiteralPath $configPath -Raw
$outputSetting = 'screenshot_dir = "' + $workPath.Replace('\', '/') + '"'
if ($configText -match '(?m)^\s*screenshot_dir\s*=') {
    $configText = $configText -replace '(?m)^\s*screenshot_dir\s*=.*$', $outputSetting
} elseif ($configText -match '(?m)^\[paths\]\s*$') {
    $configText = $configText -replace '(?m)^\[paths\]\s*$', ("[paths]`n" + $outputSetting)
} else {
    $configText += "`n[paths]`n$outputSetting`n"
}
Set-Content -LiteralPath $probeConfig -Value $configText

Add-Type -Namespace GaussianResizeCheck -Name Windows -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr window, int x, int y, int width, int height, bool repaint);
[DllImport("user32.dll")] public static extern bool EnumWindows(Enumerate callback, IntPtr parameter);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rectangle);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int length);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
public delegate bool Enumerate(IntPtr window, IntPtr parameter);
public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
'@
$start = [System.Diagnostics.ProcessStartInfo]::new($executable)
$start.WorkingDirectory = $workPath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Environment['VK_LAYER_ENABLES'] = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
# F12 ends capture mode after the requested resizes; a fast cloud must not exhaust a small frame budget first.
foreach ($argument in @('--config', $probeConfig, '--capture-frames', '1000000')) { $start.ArgumentList.Add($argument) }
$startedAt = Get-Date
$process = [System.Diagnostics.Process]::Start($start)
$standardOutput = $process.StandardOutput.ReadToEndAsync()
$standardError = $process.StandardError.ReadToEndAsync()
$resizeCount = 0
$lastResizeFrame = 0
$captureRequested = $false
$expectedResizes = if ($EarlyCapture) { 1 } elseif ($RapidResize) { 20 } else { 3 }
$expectedSize = [GaussianResizeCheck.Windows+Rect]::new()
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$previousDpiContext = [GaussianResizeCheck.Windows]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    while (-not $process.HasExited -and $timer.Elapsed.TotalSeconds -lt 55) {
        $logPath = Join-Path $workPath 'debug.log'
        $logFile = Get-Item -LiteralPath $logPath -ErrorAction SilentlyContinue
        # Repeated runs must wait for this process's log rather than count frames from the previous run.
        if ($logFile -and $logFile.LastWriteTime -ge $startedAt) {
            $costs = @(Select-String -LiteralPath $logPath -Pattern 'gaussian frame cost:')
            $frameThreshold = if ($RapidResize -or $resizeCount -eq 0) { 8 } else { $lastResizeFrame + 16 }
            if ($resizeCount -lt $expectedResizes -and $costs.Count -ge $frameThreshold) {
                $script:renderWindow = [IntPtr]::Zero
                $callback = [GaussianResizeCheck.Windows+Enumerate] {
                    param($window, $parameter)
                    $owner = [uint32]0
                    [void][GaussianResizeCheck.Windows]::GetWindowThreadProcessId($window, [ref]$owner)
                    if ($owner -eq $process.Id) {
                        $windowClass = [System.Text.StringBuilder]::new(256)
                        [void][GaussianResizeCheck.Windows]::GetClassName($window, $windowClass, 256)
                        $rectangle = [GaussianResizeCheck.Windows+Rect]::new()
                        [void][GaussianResizeCheck.Windows]::GetClientRect($window, [ref]$rectangle)
                        # GLFW's message window has the same class, but no drawable client area.
                        if ($windowClass.ToString() -eq 'GLFW30' -and $rectangle.Right -gt 0 -and $rectangle.Bottom -gt 0) {
                            $script:renderWindow = $window
                            return $false
                        }
                    }
                    return $true
                }
                [void][GaussianResizeCheck.Windows]::EnumWindows($callback, [IntPtr]::Zero)
                if ($script:renderWindow -eq [IntPtr]::Zero) { throw 'No GLFW render window for this process' }
                $sizeStep = if ($RapidResize) { $resizeCount % 3 } else { $resizeCount }
                if (-not [GaussianResizeCheck.Windows]::MoveWindow($script:renderWindow, 80, 80, $Width + $sizeStep * 112, $Height + $sizeStep * 70, $true)) {
                    throw 'MoveWindow failed'
                }
                if (-not [GaussianResizeCheck.Windows]::GetClientRect($script:renderWindow, [ref]$expectedSize)) { throw 'Cannot read resized client dimensions' }
                $resizeCount++
                $lastResizeFrame = $costs.Count
                Write-Output "Resize $resizeCount after $($costs.Count) frames: $($expectedSize.Right)x$($expectedSize.Bottom) client pixels"
            }
            if (-not $captureRequested -and $resizeCount -eq $expectedResizes -and ($EarlyCapture -or $costs.Count -ge $lastResizeFrame + 16)) {
                # F12 uses the existing capture path and quits capture mode as soon as the request is served.
                if (-not [GaussianResizeCheck.Windows]::PostMessage($script:renderWindow, 0x0100, [IntPtr]0x7B, [IntPtr]0x00580001)) { throw 'Cannot request the resized screenshot' }
                $captureRequested = $true
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $process.HasExited) { throw 'Resize check timed out; inspect debug.log' }
} finally {
    if (-not $process.HasExited) { $process.Kill($true) }
    $process.WaitForExit()
    if ($previousDpiContext -ne [IntPtr]::Zero) { [void][GaussianResizeCheck.Windows]::SetThreadDpiAwarenessContext($previousDpiContext) }
    Set-Content -LiteralPath (Join-Path $workPath 'stdout.log') -Value $standardOutput.GetAwaiter().GetResult()
    Set-Content -LiteralPath (Join-Path $workPath 'stderr.log') -Value $standardError.GetAwaiter().GetResult()
}
if ($process.ExitCode -ne 0) { throw "Renderer exited with $($process.ExitCode); inspect debug.log" }
if ($resizeCount -ne $expectedResizes) { throw "Only $resizeCount of $expectedResizes resizes ran" }
$log = Get-Content -LiteralPath (Join-Path $workPath 'debug.log') -Raw
if ($log -notmatch 'validation layers enabled') { throw 'The config must enable validation_layers' }
if ($log -notmatch 'recreat') { throw 'No swapchain rebuild was recorded' }
if ($log -match '(?m)^\[ERROR\]|Validation Error|VUID-') { throw 'Validation or rendering reported an error; inspect debug.log' }
$screenshot = Get-ChildItem -LiteralPath $workPath -Filter 'screenshot_*.png' -File |
    Where-Object { $_.LastWriteTime -ge $startedAt } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $screenshot) { throw 'No new screenshot was produced' }
$bytes = [System.IO.File]::ReadAllBytes($screenshot.FullName)
$imageWidth = [System.Net.IPAddress]::NetworkToHostOrder([System.BitConverter]::ToInt32($bytes, 16))
$imageHeight = [System.Net.IPAddress]::NetworkToHostOrder([System.BitConverter]::ToInt32($bytes, 20))
if ($imageWidth -ne $expectedSize.Right -or $imageHeight -ne $expectedSize.Bottom) { throw 'Screenshot dimensions do not match the resized client area' }
Write-Output "PASS: $resizeCount resize(s), clean synchronization validation, $imageWidth x $imageHeight output"
Write-Output $screenshot.FullName
