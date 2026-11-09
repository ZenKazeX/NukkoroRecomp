# Renders the Hungarian 50/60 Hz screen picture (palSel/PalSel<lang>.txd, texture "screentest", 512 x 512) as raw RGB
# (512*512*3 bytes) for tools/hungarian/make_palsel.py. The original is a picture: a red outlined techno face for the
# title (Unispace Bold here) and a white condensed face for the lines (Bahnschrift Condensed here (it has the double acute)); the text is placed on the
# original's bands (title 6-34 / 42-67, choices 89-112, lines 123-149, 192-218, 261-287, 330-356) centred on x 256.
# Needs Windows (System.Drawing). The character codes are given as numbers: this file stays ASCII.
param([string]$Out = "palsel_hu.raw")
Add-Type -AssemblyName System.Drawing

function U([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }
$e_acute = 0xE9; $o_acute = 0xF3; $o_dacute = 0x151; $i_acute = 0xED; $a_acute = 0xE1; $u_dacute = 0x171

# The texts (Hungarian). Title: "Szeretned 60Hz-es" / "modban megjeleniteni?"
$title1 = "Szeretn" + (U @($e_acute)) + "d 60Hz-es"
$title2 = "m" + (U @($o_acute)) + "dban megjelen" + (U @($i_acute)) + "teni?"
$yes = "Igen"; $no = "Nem"; $test = "Teszt"
$s60 = "A k" + (U @($e_acute)) + "perny" + (U @($o_dacute)) + " 60Hz-es m" + (U @($o_acute)) + "dra lett " + (U @($a_acute)) + "ll" + (U @($i_acute)) + "tva."
$s50 = "A k" + (U @($e_acute)) + "perny" + (U @($o_dacute)) + " 50Hz-es m" + (U @($o_acute)) + "dra lett " + (U @($a_acute)) + "ll" + (U @($i_acute)) + "tva."
$t60 = "60Hz-es m" + (U @($o_acute)) + "d tesztel" + (U @($e_acute)) + "se a TV-n"
$now = "Tesztel" + (U @($e_acute)) + "s folyamatban...."

$bmp = New-Object System.Drawing.Bitmap 512, 512
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Black)
$g.SmoothingMode = 'AntiAlias'
$fmt = [System.Drawing.StringFormat]::GenericTypographic

# A string as a path at a pixel size, its left edge at x and its baseline at y.
function Path-Of([string]$text, [string]$family, [bool]$bold, [double]$size, [double]$x, [double]$baseline) {
    $fam = New-Object System.Drawing.FontFamily($family)
    $style = if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
    $ascent = $fam.GetCellAscent($style) / $fam.GetEmHeight($style) * $size
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString($text, $fam, [int]$style, [single]$size, (New-Object System.Drawing.PointF([single]$x, [single]($baseline - $ascent))), $fmt)
    return $path
}
# The size at which a reference string's ink is `cap` pixels tall.
function Size-For([string]$family, [bool]$bold, [double]$cap, [string]$ref) {
    $p = Path-Of $ref $family $bold 100 0 100
    return 100.0 * $cap / $p.GetBounds().Height
}
# Draws `text` centred on cx (or left at x when cx < 0), shrinking to `maxw`.
function Draw-Text([string]$text, [string]$family, [bool]$bold, [double]$size, [double]$cx, [double]$left, [double]$baseline, [double]$maxw, [bool]$title) {
    $p = Path-Of $text $family $bold $size 0 $baseline
    $b = $p.GetBounds()
    if ($b.Width -gt $maxw) { $size = $size * $maxw / $b.Width; $p = Path-Of $text $family $bold $size 0 $baseline; $b = $p.GetBounds() }
    $x = if ($cx -ge 0) { $cx - ($b.X + $b.Width / 2) } else { $left - $b.X }
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([single]$x, 0)
    $p.Transform($m)
    if ($title) {
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(72, 40, 40), 3.0)
        $pen.LineJoin = 'Round'
        $g.DrawPath($pen, $p)
        $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(204, 0, 0))), $p)
    } else {
        $g.FillPath([System.Drawing.Brushes]::White, $p)
    }
}

$titleSize = Size-For "Unispace" $true 22 "WHd"
$bodySize = Size-For "Bahnschrift SemiLight Condensed" $false 19 "dh"
$choiceSize = Size-For "Bahnschrift SemiBold Condensed" $false 22 "YT"

Draw-Text $title1 "Unispace" $true $titleSize 257 0 29 440 $true
Draw-Text $title2 "Unispace" $true $titleSize 257 0 66 440 $true
Draw-Text $yes "Bahnschrift SemiBold Condensed" $false $choiceSize -1 20 111 70 $false
Draw-Text $no "Bahnschrift SemiBold Condensed" $false $choiceSize -1 102 111 60 $false
Draw-Text $test "Bahnschrift SemiBold Condensed" $false $choiceSize -1 180 111 70 $false
Draw-Text $s60 "Bahnschrift SemiLight Condensed" $false $bodySize 257 0 143 440 $false
Draw-Text $s50 "Bahnschrift SemiLight Condensed" $false $bodySize 257 0 212 440 $false
Draw-Text $t60 "Bahnschrift SemiLight Condensed" $false $bodySize 257 0 281 440 $false
Draw-Text $now "Bahnschrift SemiLight Condensed" $false $bodySize 257 0 350 440 $false

$bytes = New-Object byte[] (512 * 512 * 3)
$i = 0
for ($y = 0; $y -lt 512; $y++) {
    for ($x = 0; $x -lt 512; $x++) {
        $c = $bmp.GetPixel($x, $y)
        $bytes[$i] = $c.R; $bytes[$i + 1] = $c.G; $bytes[$i + 2] = $c.B
        $i += 3
    }
}
[System.IO.File]::WriteAllBytes($Out, $bytes)
"sizes: title $titleSize body $bodySize choice $choiceSize"
