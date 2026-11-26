param([Parameter(Mandatory)] [string]$Frames, [string]$Art = $PSScriptRoot)
# Packs the stick frames (render_top.py "sticks", in <out dir>\sticks) into art/top_stick_<main|c>.png: the middle at the
# top left, then a row per distance (1..4), a column per direction (0..15); cropped to what the frames use, faint shadow
# cleared, edges faded. Prints the kStickCaps lines for art/hotspots.inc (its last number, the radius of the cap's top,
# stays as it is: 0.0620 and 0.0340). The frames' origin in the picture (66, 105 and 496, 317) is STICK_AREA in
# render_top.py, in pixels.
#   powershell -File pack_sticks.ps1 -Frames <out dir>\sticks
Add-Type -AssemblyName System.Drawing
function Get-Bytes($bmp) { $r = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height; $d = $bmp.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb); $bytes = New-Object byte[] ($d.Stride * $bmp.Height); [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $bytes, 0, $bytes.Length); $bmp.UnlockBits($d); return @($bytes, $d.Stride) }
function Get-Centroid($file) { $im = [System.Drawing.Bitmap]::FromFile($file); $a = Get-Bytes $im; $b = $a[0]; $st = $a[1]; $sx = 0.0; $sy = 0.0; $sw = 0.0; for ($y = 0; $y -lt $im.Height; $y++) { for ($x = 0; $x -lt $im.Width; $x++) { $i = $y * $st + $x * 4; if ($b[$i + 3] -gt 128) { $lum = ($b[$i] + $b[$i + 1] + $b[$i + 2]); $w = [double]$lum * $lum * $lum; $sx += $x * $w; $sy += $y * $w; $sw += $w } } }; $im.Dispose(); return @(($sx / $sw), ($sy / $sw)) }
$names = @('0') + (1..4 | ForEach-Object { $r = $_; 0..15 | ForEach-Object { "${r}_$_" } })
$inv = [Globalization.CultureInfo]::InvariantCulture
foreach ($k in @(@{n = 'main'; ox = 66; oy = 105; id = 104; key = 'stick' }, @{n = 'c'; ox = 496; oy = 317; id = 105; key = 'cstick' })) {
  $minx = 9999; $miny = 9999; $maxx = -1; $maxy = -1
  foreach ($nm in $names) {
    $im = [System.Drawing.Bitmap]::FromFile("$Frames\$($k.n)_$nm.png"); $W = $im.Width; $H = $im.Height; $a = Get-Bytes $im; $bytes = $a[0]; $st = $a[1]
    for ($y = 0; $y -lt $H; $y++) { $row = $y * $st; for ($x = 0; $x -lt $W; $x++) { if ($bytes[$row + $x * 4 + 3] -gt 8) { if ($x -lt $minx) { $minx = $x }; if ($x -gt $maxx) { $maxx = $x }; if ($y -lt $miny) { $miny = $y }; if ($y -gt $maxy) { $maxy = $y } } } }
    $im.Dispose()
  }
  $minx = [Math]::Max(0, $minx - 1); $miny = [Math]::Max(0, $miny - 1); $maxx = [Math]::Min($W - 1, $maxx + 1); $maxy = [Math]::Min($H - 1, $maxy + 1); $cw = $maxx - $minx + 1; $ch = $maxy - $miny + 1
  $sheet = New-Object System.Drawing.Bitmap (16 * $cw), (5 * $ch), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb); $g = [System.Drawing.Graphics]::FromImage($sheet); $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
  foreach ($nm in $names) {
    if ($nm -eq '0') { $col = 0; $row = 0 } else { $p = $nm.Split('_'); $row = [int]$p[0]; $col = [int]$p[1] }
    $im = [System.Drawing.Bitmap]::FromFile("$Frames\$($k.n)_$nm.png"); $g.DrawImage($im, (New-Object System.Drawing.Rectangle ($col * $cw), ($row * $ch), $cw, $ch), (New-Object System.Drawing.Rectangle $minx, $miny, $cw, $ch), [System.Drawing.GraphicsUnit]::Pixel); $im.Dispose()
  }
  $g.Dispose()
  # The shadow catcher leaves a faint shadow everywhere: below 8 of 255 it is invisible, and clearing it keeps the sheet small.
  $rect = New-Object System.Drawing.Rectangle 0, 0, $sheet.Width, $sheet.Height
  $d = $sheet.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $all = New-Object byte[] ($d.Stride * $sheet.Height); [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $all, 0, $all.Length)
  # And a frame's faint shadow can reach the edge of the area rendered: faded out over the last 16 pixels before that
  # edge, so it never ends in a straight line.
  $fade = 16.0
  for ($py = 0; $py -lt $sheet.Height; $py++) {
    $ay = ($py % $ch) + $miny; $ey = [Math]::Min($ay, $H - 1 - $ay)
    for ($px = 0; $px -lt $sheet.Width; $px++) {
      $i = $py * $d.Stride + $px * 4 + 3
      if ($all[$i] -eq 0) { continue }
      $ax = ($px % $cw) + $minx; $e = [Math]::Min($ey, [Math]::Min($ax, $W - 1 - $ax))
      if ($e -lt $fade) { $all[$i] = [byte]([Math]::Floor($all[$i] * [Math]::Max(0.0, $e) / $fade)) }
      if ($all[$i] -lt 8) { $all[$i] = 0; $all[$i - 1] = 0; $all[$i - 2] = 0; $all[$i - 3] = 0 }
    }
  }
  [Runtime.InteropServices.Marshal]::Copy($all, 0, $d.Scan0, $all.Length); $sheet.UnlockBits($d)
  $sheet.Save("$Art\top_stick_$($k.n).png", [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
  "$($k.n): cell ${cw}x${ch}, sheet $((Get-Item "$Art\top_stick_$($k.n).png").Length) bytes"
  "    {""$($k.key)"", $($k.id), " + (($k.ox + $minx) / 1000.0).ToString('0.0000', $inv) + "f, " + (($k.oy + $miny) / 659.0).ToString('0.0000', $inv) + "f, " + ($cw / 1000.0).ToString('0.0000', $inv) + "f, " + ($ch / 659.0).ToString('0.0000', $inv) + "f, " + $(if ($k.n -eq 'main') { '0.0620' } else { '0.0340' }) + "f},"
}
