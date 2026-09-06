<#
.SYNOPSIS
    Converts the raw generated art in workshop\image\ into the deliverables:
      steam_preview_src.png -> steam_preview.jpg   square, <= 1024x1024, JPEG ~85, MUST be < 1 MB
      github_social_src.png -> github_social.png   1280x640, centre-crop
    Uses System.Drawing only (no new dependency). The *_src.png files are the
    sources; re-run after regenerating them.
#>
[CmdletBinding()]
param(
    [int] $JpegQuality = 85
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here = $PSScriptRoot

function Resize-Cover([System.Drawing.Image] $src, [int] $w, [int] $h) {
    # Scale to cover w x h, then crop the centre.
    $scale = [Math]::Max($w / $src.Width, $h / $src.Height)
    $sw = [int][Math]::Ceiling($src.Width * $scale)
    $sh = [int][Math]::Ceiling($src.Height * $scale)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($src, [int](($w - $sw) / 2), [int](($h - $sh) / 2), $sw, $sh)
    $g.Dispose()
    return $bmp
}

function Save-Jpeg([System.Drawing.Image] $img, [string] $path, [int] $quality) {
    $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    $p = New-Object System.Drawing.Imaging.EncoderParameters 1
    $p.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long] $quality)
    $img.Save($path, $codec, $p)
    $p.Dispose()
}

# --- steam_preview.jpg: square, max 1024, < 1 MB ------------------------------------------
$srcPath = Join-Path $here 'steam_preview_src.png'
if (-not (Test-Path $srcPath)) { throw "Missing $srcPath" }
$src  = [System.Drawing.Image]::FromFile($srcPath)
$side = [Math]::Min(1024, [Math]::Min($src.Width, $src.Height))
$sq   = Resize-Cover $src $side $side
$src.Dispose()
$out = Join-Path $here 'steam_preview.jpg'
$q = $JpegQuality
do {
    Save-Jpeg $sq $out $q
    $bytes = (Get-Item $out).Length
    if ($bytes -lt 1MB) { break }
    $q -= 5   # Steam rejects >= 1 MB; step the quality down until it fits.
} while ($q -ge 50)
$sq.Dispose()
if ($bytes -ge 1MB) { throw "steam_preview.jpg is still $bytes bytes at quality $q; shrink the source." }
Write-Host ("steam_preview.jpg  {0}x{0}  q{1}  {2:N0} bytes" -f $side, $q, $bytes)

# --- github_social.png: 1280x640 centre-crop ------------------------------------------------
$srcPath = Join-Path $here 'github_social_src.png'
if (-not (Test-Path $srcPath)) { throw "Missing $srcPath" }
$src = [System.Drawing.Image]::FromFile($srcPath)
$gh  = Resize-Cover $src 1280 640
$src.Dispose()
$out = Join-Path $here 'github_social.png'
$gh.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$gh.Dispose()
Write-Host ("github_social.png  1280x640  {0:N0} bytes" -f (Get-Item $out).Length)
