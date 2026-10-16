<#
.SYNOPSIS
    Fetch the open-source Gaussian-splat asset the viewer is verified with.

.DESCRIPTION
    The acceptance model is NVIDIA's "Bouquet of Flowers" scene, published for the vk_gaussian_splatting
    sample and released under CC BY-SA 4.0 (courtesy Jean-Eudes Marvie):
    http://developer.download.nvidia.com/ProGraphics/nvpro-samples/flowers_1.zip

    IT IS NOT COMMITTED, and that is deliberate: it is a 133 MB binary with its own licence, and the repository
    is where code lives. This script fetches and unpacks it into the build tree (which is ignored), so the
    viewer can be run against the real thing, and the README of the viewer plan records where it came from.

    Measured on this machine: 119.7 MB downloaded in 9 s through the local proxy, unpacking to
    flowers_1.ply - 133.2 MB, 562,974 splats, 62 float properties per vertex (xyz, normals, f_dc, 45 f_rest,
    opacity, scale, rotation) in binary little endian, which deren.gaussian::load_ply reads in 0.4 s.

.EXAMPLE
    ./fetch_gaussian_viewer_asset.ps1 -BuildDir build-release-dyn-clang64
    ./fetch_gaussian_viewer_asset.ps1 -BuildDir build-release-dyn-clang64 -Proxy http://127.0.0.1:12000
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-release-dyn-clang64",
    # The local proxy, when the machine needs one: the NVIDIA host is plain HTTP, which git's own proxy
    # configuration does not cover.
    [string]$Proxy = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root $BuildDir
$work = Join-Path $target "gaussian-viewer"
$zip = Join-Path $work "flowers_1.zip"
$out = Join-Path $work "flowers"
$ply = Join-Path $out "flowers_1\flowers_1.ply"
$url = "http://developer.download.nvidia.com/ProGraphics/nvpro-samples/flowers_1.zip"

New-Item -ItemType Directory -Force $work | Out-Null
if ((Test-Path $ply) -and -not $Force) {
    Write-Host "already present: $ply"
    exit 0
}

$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { throw "curl.exe is required to fetch the asset" }
$arguments = @("-L", "--max-time", "900", "-o", $zip)
if ($Proxy) { $arguments += @("--proxy", $Proxy) }
$arguments += $url
& $curl @arguments
if ($LASTEXITCODE -ne 0) { throw "download failed with exit $LASTEXITCODE" }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath $zip -DestinationPath $out -Force
if (-not (Test-Path $ply)) { throw "the archive did not contain flowers_1.ply" }
$size = (Get-Item $ply).Length / 1MB
Write-Host ("{0}: {1:N1} MB - CC BY-SA 4.0, courtesy Jean-Eudes Marvie (see {2})" -f $ply, $size, $url)
