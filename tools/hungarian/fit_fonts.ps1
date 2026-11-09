# Renders one text in many installed fonts, each scaled to the same ink box (x0,y0,x1,y1 of the original picture's
# text), one 512 x 40 strip per font, as raw gray (512*40*N bytes) for fit_fonts.py to rank against the original.
param([string]$Out, [string]$Text, [int]$X0, [int]$Width, [int]$Height, [string]$Families, [int]$Bold = 0)
Add-Type -AssemblyName System.Drawing
$fams = $Families.Split('|')
$bytes = New-Object byte[] (512 * 40 * $fams.Count)
$k = 0
foreach ($name in $fams) {
    $bmp = New-Object System.Drawing.Bitmap 512, 40
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black); $g.SmoothingMode = 'AntiAlias'
    try {
        $fam = New-Object System.Drawing.FontFamily($name)
        $style = if ($Bold -ne 0) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $path.AddString($Text, $fam, [int]$style, 100, (New-Object System.Drawing.PointF(0, 0)), [System.Drawing.StringFormat]::GenericTypographic)
        $b = $path.GetBounds()
        $m = New-Object System.Drawing.Drawing2D.Matrix
        $m.Translate([single](-$b.X), [single](-$b.Y))
        $path.Transform($m)
        $m2 = New-Object System.Drawing.Drawing2D.Matrix
        $m2.Scale([single]($Width / $b.Width), [single]($Height / $b.Height))
        $path.Transform($m2)
        $m3 = New-Object System.Drawing.Drawing2D.Matrix
        $m3.Translate([single]$X0, 5)
        $path.Transform($m3)
        $g.FillPath([System.Drawing.Brushes]::White, $path)
    } catch { }
    for ($y = 0; $y -lt 40; $y++) { for ($x = 0; $x -lt 512; $x++) { $bytes[$k * 512 * 40 + $y * 512 + $x] = $bmp.GetPixel($x, $y).G } }
    $k++
}
[System.IO.File]::WriteAllBytes($Out, $bytes)
