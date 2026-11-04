param(
    [string]$BuildDir = 'build',
    [string]$Motion = 'asset/motion/bibideba_laevatain/motion.gltf'
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildPath = (Resolve-Path (Join-Path $repoRoot $BuildDir)).Path
$motionPath = (Resolve-Path (Join-Path $repoRoot $Motion)).Path
$modelPath = Join-Path $repoRoot 'asset/laevatain/laevatain_game_toon_preview.glb'
$previewDir = Join-Path $repoRoot 'tmp/motion-preview'
New-Item -ItemType Directory -Path $previewDir -Force | Out-Null
$shaderPath = (Join-Path $buildPath 'shaders').Replace('\', '/')
$modelToml = $modelPath.Replace('\', '/')
$screenshotPath = $previewDir.Replace('\', '/')
$environmentPath = Join-Path $buildPath 'chars/laevatain_preview_constant.hdr'
$environmentSetting = ''
if (Test-Path -LiteralPath $environmentPath) {
    $environmentSetting = 'environment_hdr="' + $environmentPath.Replace('\', '/') + '"'
}
$configPath = Join-Path $previewDir 'live.toml'
@"
model="$modelToml"
[paths]
shaders_dir="$shaderPath"
screenshot_dir="$screenshotPath"
[render]
camera_pose=[0.0,0.0,2.6,0.0,-0.6,0.0]
window_width=1000
window_height=1000
vsync=true
shadow=true
character_forward=true
game_toon=true
taa=false
fxaa=true
animation_time=-1.0
[gui]
show=true
[lighting]
sun_direction=[0.0,0.25,1.0]
$environmentSetting
environment_intensity=1.0
environment_background=1
environment_color=[0.18,0.18,0.18]
"@ | Set-Content -LiteralPath $configPath -Encoding utf8
Push-Location $buildPath
try {
    & (Join-Path $buildPath 'deren.exe') --config $configPath --motion $motionPath
    if ($LASTEXITCODE -ne 0) { throw "Motion preview exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
