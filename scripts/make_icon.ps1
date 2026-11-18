# Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
# Writes res/birdie.ico: the same bird as make_bird_icon() in src/app/app.cpp (flight path vector symbol,
# apricot #f2b48a over a soft dark outline), one PNG image per size. Rerun after changing the drawing.
param([string]$Out = (Join-Path $PSScriptRoot '..\res\birdie.ico'))

Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256

function Draw-Bird([int]$n) {
  $bmp = New-Object System.Drawing.Bitmap $n, $n, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  $k = $n / 64.0
  $draw = {
    param($pen)
    $g.DrawEllipse($pen, [single]((32 - 11) * $k), [single]((36 - 11) * $k), [single](22 * $k), [single](22 * $k))
    $g.DrawLine($pen, [single](21 * $k), [single](36 * $k), [single](3 * $k), [single](36 * $k))
    $g.DrawLine($pen, [single](43 * $k), [single](36 * $k), [single](61 * $k), [single](36 * $k))
    $g.DrawLine($pen, [single](32 * $k), [single](25 * $k), [single](32 * $k), [single](11 * $k))
  }
  $outline = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(170, 0, 0, 0)), ([single](10 * $k))
  $fg = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0xf2, 0xb4, 0x8a)), ([single](5.5 * $k))
  foreach ($p in $outline, $fg) { $p.StartCap = 'Round'; $p.EndCap = 'Round'; & $draw $p }
  $g.Dispose(); $outline.Dispose(); $fg.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return , $ms.ToArray()
}

$images = foreach ($n in $sizes) { , (Draw-Bird $n) }
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)  # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                     # ICONDIRENTRY, PNG payload
  $n = $sizes[$i]; $len = $images[$i].Length
  $w.Write([byte]($n % 256)); $w.Write([byte]($n % 256)); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$len); $w.Write([uint32]$offset)
  $offset += $len
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out), $ms.ToArray())
"wrote $Out ($($ms.Length) bytes, sizes $($sizes -join ' '))"
