# Makes build\mua3.ico (the executables' icon) from your installed game's icon (game\control\icon_AmericanEnglish.dat,
# a 256x256 JPEG). It is game data, so the icon is generated locally and never committed; CMake embeds it when present.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make_icon.ps1 [-Game <game folder>] [-Out <ico>]
param([string]$Game = '', [string]$Out = '')
$root = Split-Path $PSScriptRoot -Parent
if (-not $Game) { $Game = Join-Path $root 'GameFiles\game' }
if (-not $Out) { $Out = Join-Path $root 'build\mua3.ico' }
$src = Join-Path $Game 'control\icon_AmericanEnglish.dat'
if (-not (Test-Path $src)) { throw "game icon not found: $src" }
Add-Type -AssemblyName System.Drawing
$img = [Drawing.Image]::FromFile($src)
$sizes = 256, 128, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object Drawing.Bitmap $s, $s
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($img, 0, 0, $s, $s)
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$img.Dispose()
# ICONDIR + one ICONDIRENTRY per size, PNG-compressed images after them
$fs = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)) # 256 is stored as 0
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $fs.ToArray())
"wrote $Out ($($fs.Length) bytes, sizes $($sizes -join ', '))"
