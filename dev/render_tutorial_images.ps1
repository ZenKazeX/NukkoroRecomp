<#
.SYNOPSIS
  Renders the pictures of docs/MOD_MANAGER_TUTORIAL.md and docs/SETTINGS_TUTORIAL.md from the real programs.

.DESCRIPTION
  Every picture is the program's own window, written by the program itself with its --snapshot switch (PrintWindow of
  its window, as it is drawn): ModManager.exe --snapshot/--select, Settings.exe --snapshot/--page. Nothing is drawn
  into a picture except the red marks and numbered labels that point at the controls a step talks about (the style of
  the install pictures in README.md). Where a step needs the program in a state, the state is made the way a player
  makes it: the Mod Manager's own command line installs, ticks, orders and sets options (the window shows the same
  mods.json), and a button of the Settings window is pressed (BM_CLICK) before the picture is taken.

  The places of the marks come from the live window: while the program runs, before its picture is written, this
  script reads the position of every control (EnumChildWindows), so a mark sits where the control really is.

  Everything happens in a scratch folder (-Work): its own copy of the two programs, its own mods folder and settings
  files. The example mods are made there from the game's own files (a song put in place of another song, a generated
  picture as a texture); nothing of the game or of anyone's mod is put into the repository. No other folder is read
  or written, and no installed game is touched.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File dev\render_tutorial_images.ps1 `
      -Bin .build\tools\bin\Release -Game D:\Dumps\ShadowPAL -Work D:\scratch\tutorial-render
#>
param(
    [Parameter(Mandatory = $true)][string]$Bin,   # folder with Settings.exe, ModManager.exe and mods-data\ (a build)
    [Parameter(Mandatory = $true)][string]$Game,  # the extracted PAL disc (the folder with files\): read only
    [Parameter(Mandatory = $true)][string]$Work,  # scratch folder (emptied first)
    [string]$Out = '',                            # default: docs\assets of this repository
    [string]$Only = ''                            # regex of picture names to render (default: all)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class TutWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
    // The window's visible frame (without the invisible resize borders), on the screen.
    public static RECT Frame(IntPtr h) {
        RECT r;
        if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r);
        return r;
    }
    // Every visible control below the window: id, class, text and its rectangle on the screen, one per line.
    public static string[] Controls(IntPtr top) {
        var lines = new List<string>();
        EnumChildWindows(top, (h, l) => {
            if (!IsWindowVisible(h)) return true;
            var t = new StringBuilder(512); GetWindowText(h, t, 512);
            var c = new StringBuilder(128); GetClassName(h, c, 128);
            RECT r; GetWindowRect(h, out r);
            lines.Add(GetDlgCtrlID(h) + "\t" + c + "\t" + t.ToString().Replace("\t", " ").Replace("\r", " ").Replace("\n", " ") +
                      "\t" + r.L + "\t" + r.T + "\t" + r.R + "\t" + r.B);
            return true;
        }, IntPtr.Zero);
        return lines.ToArray();
    }
    // The first control of a class below the window.
    public static IntPtr Child(IntPtr top, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(top, (h, l) => {
            var c = new StringBuilder(128); GetClassName(h, c, 128);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // A list view's row height: the list's own size for two rows less its size for one (LVM_APPROXIMATEVIEWRECT).
    public static int RowHeight(IntPtr list) {
        long one = SendMessage(list, 0x1040, (IntPtr)1, (IntPtr)(-1)).ToInt64(), two = SendMessage(list, 0x1040, (IntPtr)2, (IntPtr)(-1)).ToInt64();
        return (int)(((two >> 16) & 0xFFFF) - ((one >> 16) & 0xFFFF));
    }
}
'@

$Work = [IO.Path]::GetFullPath($Work)
if ($Out -eq '') { $Out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\docs\assets' }
$Out = [IO.Path]::GetFullPath($Out)
$Game = [IO.Path]::GetFullPath($Game)
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$binCopy = Join-Path $Work 'Shadow the Hedgehog'      # the programs, as in an install folder (and their appearance.cfg)
New-Item -ItemType Directory -Force $binCopy | Out-Null
Copy-Item (Join-Path $Bin 'Settings.exe'), (Join-Path $Bin 'ModManager.exe') $binCopy
Copy-Item (Join-Path $Bin 'mods-data') $binCopy -Recurse
$mods = Join-Path $binCopy 'mods'
$downloads = Join-Path $Work 'Downloads'
New-Item -ItemType Directory -Force $mods, $downloads | Out-Null
function Write-Text([string]$path, [string]$text) { [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false)) }
function Set-Look([string]$theme, [int]$zoom) { Write-Text (Join-Path $binCopy 'appearance.cfg') "theme = $theme`r`nzoom = $zoom`r`n" }

# ---- running a program and reading its window ------------------------------------------------------------------------

# Starts the program, waits for its window, runs $act (the window handle as its argument) and reads where every control
# is; the program writes its picture and ends by itself. Returns the window's frame, client origin and controls, in the
# picture's pixels.
function Invoke-SnapshotOnce([string]$exe, [string[]]$arguments, [string]$png, [scriptblock]$act = $null) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.WorkingDirectory = $binCopy
    $psi.UseShellExecute = $false
    $p = [Diagnostics.Process]::Start($psi)
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 200 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 25; $p.Refresh(); $h = $p.MainWindowHandle }
    if ($h -eq [IntPtr]::Zero) { throw "no window: $exe $($psi.Arguments)" }
    Start-Sleep -Milliseconds 300
    if ($act) { & $act $h; Start-Sleep -Milliseconds 300 }
    $win = New-Object TutWin+RECT; [void][TutWin]::GetWindowRect($h, [ref]$win)
    $frame = [TutWin]::Frame($h)
    $origin = New-Object TutWin+POINT; [void][TutWin]::ClientToScreen($h, [ref]$origin)
    $controls = foreach ($line in [TutWin]::Controls($h)) {
        $f = $line -split "`t"
        [pscustomobject]@{ Id = [int]$f[0]; Class = $f[1]; Text = $f[2]
            X = [int]$f[3] - $win.L; Y = [int]$f[4] - $win.T; W = [int]$f[5] - [int]$f[3]; H = [int]$f[6] - [int]$f[4] }
    }
    $listView = [TutWin]::Child($h, 'SysListView32')
    $rowH = if ($listView -ne [IntPtr]::Zero) { [TutWin]::RowHeight($listView) } else { 0 }
    if (-not $p.WaitForExit(20000)) { $p.Kill(); throw "the program did not end: $exe" }
    if (-not (Test-Path $png)) { throw "no picture: $png" }
    return [pscustomobject]@{
        Frame = [Drawing.Rectangle]::FromLTRB($frame.L - $win.L, $frame.T - $win.T, $frame.R - $win.L, $frame.B - $win.T)
        Client = New-Object Drawing.Point ($origin.X - $win.L), ($origin.Y - $win.T)
        Controls = @($controls); Png = $png; RowH = $rowH }
}

# A window's picture is sometimes taken before every child control has painted (a missing "Details" title, an empty
# details pane or status line were seen). Each picture is therefore taken three times and the fullest one kept: the
# one with the most drawn (the largest PNG). All three show the same state; only how much of it was painted differs.
function Invoke-Snapshot([string]$exe, [string[]]$arguments, [string]$png, [scriptblock]$act = $null, [scriptblock]$before = $null) {
    $best = $null; $bestSize = -1
    for ($try = 1; $try -le 3; $try++) {
        if ($before) { & $before }   # each try starts from the same files
        $file = $png -replace '\.png$', "-try$try.png"
        $shot = Invoke-SnapshotOnce $exe @($arguments | ForEach-Object { if ($_ -eq $png) { $file } else { $_ } }) $file $act
        $size = (Get-Item $file).Length
        if ($size -gt $bestSize) { $best = $shot; $bestSize = $size }
    }
    Copy-Item $best.Png $png -Force
    $best.Png = $png
    return $best
}
function Find-Control($shot, [string]$text, [string]$class = '') {
    $c = $shot.Controls | Where-Object { $_.Text -eq $text -and ($class -eq '' -or $_.Class -eq $class) } | Select-Object -First 1
    if (-not $c) { throw "no control '$text' in $($shot.Png)" }
    return New-Object Drawing.Rectangle $c.X, $c.Y, $c.W, $c.H
}
function Find-Id($shot, [int]$id) {
    $c = $shot.Controls | Where-Object { $_.Id -eq $id } | Select-Object -First 1
    if (-not $c) { throw "no control $id in $($shot.Png)" }
    return New-Object Drawing.Rectangle $c.X, $c.Y, $c.W, $c.H
}
function Find-Class($shot, [string]$class) {
    $c = $shot.Controls | Where-Object { $_.Class -eq $class } | Select-Object -First 1
    if (-not $c) { throw "no $class in $($shot.Png)" }
    return New-Object Drawing.Rectangle $c.X, $c.Y, $c.W, $c.H
}
function Grow([Drawing.Rectangle]$r, [int]$by) { return [Drawing.Rectangle]::FromLTRB($r.Left - $by, $r.Top - $by, $r.Right + $by, $r.Bottom + $by) }
function Join-Rect([Drawing.Rectangle]$a, [Drawing.Rectangle]$b) { return [Drawing.Rectangle]::Union($a, $b) }

# ---- the marks (the install pictures' style: a red frame round the control, a red label with white words, an arrow) --

$red = [Drawing.Color]::FromArgb(255, 224, 36, 40)
function New-RoundPath([Drawing.RectangleF]$r, [float]$radius) {
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $path.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $path.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# Writes the picture: the window cut to $crop (null: the whole visible window), with the marks. Each mark:
#   @{ Box = rectangle round the control; Text = '1  Click this'; At = 'right'|'left'|'above'|'below' or a point;
#      Arrow = $false to leave the arrow out; Via = points the arrow goes through first (window pixels), so it can run
#      along an empty margin instead of across the program's text }
# $extend adds that many pixels of plain background under the window, a place for labels that would otherwise cover
# the window's own bottom bar.
function Save-Marked($shot, [string]$name, $crop, [object[]]$marks, [int]$extend = 0) {
    $source = [Drawing.Bitmap]::FromFile($shot.Png)
    if ($null -eq $crop) { $crop = $shot.Frame }
    $crop = [Drawing.Rectangle]::Intersect($crop, $shot.Frame)
    $bmp = New-Object Drawing.Bitmap $crop.Width, ($crop.Height + $extend), ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::FromArgb(255, 32, 32, 32))
    $g.DrawImage($source, (New-Object Drawing.Rectangle 0, 0, $crop.Width, $crop.Height), $crop, [Drawing.GraphicsUnit]::Pixel)
    $source.Dispose()
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $font = New-Object Drawing.Font 'Segoe UI', 17, ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
    $pen = New-Object Drawing.Pen $red, 3
    $brush = New-Object Drawing.SolidBrush $red
    $white = [Drawing.Brushes]::White
    foreach ($m in $marks) {
        $box = [Drawing.RectangleF]::new($m.Box.X - $crop.X, $m.Box.Y - $crop.Y, $m.Box.Width, $m.Box.Height)
        $path = New-RoundPath $box 5
        $g.DrawPath($pen, $path)
        if (-not $m.Text) { continue }
        $size = $g.MeasureString($m.Text, $font)
        $lw = [float]([Math]::Ceiling($size.Width) + 16); $lh = [float]([Math]::Ceiling($size.Height) + 8)
        $gap = 34
        switch ($m.At) {
            'right' { $lx = $box.Right + $gap; $ly = $box.Y + $box.Height / 2 - $lh / 2 }
            'left'  { $lx = $box.X - $gap - $lw; $ly = $box.Y + $box.Height / 2 - $lh / 2 }
            'above' { $lx = $box.X + $box.Width / 2 - $lw / 2; $ly = $box.Y - $gap - $lh }
            'below' { $lx = $box.X + $box.Width / 2 - $lw / 2; $ly = $box.Bottom + $gap }
            default { $lx = [float]$m.At.X - $crop.X; $ly = [float]$m.At.Y - $crop.Y }
        }
        $lx = [Math]::Max(4, [Math]::Min($lx, $bmp.Width - $lw - 4)); $ly = [Math]::Max(4, [Math]::Min($ly, $bmp.Height - $lh - 4))
        $label = [Drawing.RectangleF]::new($lx, $ly, $lw, $lh)
        if ($m.Arrow -ne $false) {
            # From the label's nearest edge point (to the first Via point, or to the frame's middle) through the Via
            # points to the frame's nearest edge point, with a head at the frame.
            $via = @($m.Via | Where-Object { $_ } | ForEach-Object { New-Object Drawing.PointF ([float]($_.X - $crop.X)), ([float]($_.Y - $crop.Y)) })
            $aimX = if ($via.Count) { $via[0].X } else { $box.X + $box.Width / 2 }
            $aimY = if ($via.Count) { $via[0].Y } else { $box.Y + $box.Height / 2 }
            $cx = [Math]::Max($label.X, [Math]::Min($aimX, $label.Right))
            $cy = [Math]::Max($label.Y, [Math]::Min($aimY, $label.Bottom))
            $lastX = if ($via.Count) { $via[-1].X } else { $cx }; $lastY = if ($via.Count) { $via[-1].Y } else { $cy }
            $tx = [Math]::Max($box.X, [Math]::Min($lastX, $box.Right)); $ty = [Math]::Max($box.Y, [Math]::Min($lastY, $box.Bottom))
            $points = @(New-Object Drawing.PointF ([float]$cx), ([float]$cy)) + $via + @(New-Object Drawing.PointF ([float]$tx), ([float]$ty))
            $length = 0.0
            for ($i = 1; $i -lt $points.Count; $i++) { $length += [Math]::Sqrt([Math]::Pow($points[$i].X - $points[$i - 1].X, 2) + [Math]::Pow($points[$i].Y - $points[$i - 1].Y, 2)) }
            if ($length -gt 8) {
                $arrow = New-Object Drawing.Pen $red, 3
                $arrow.LineJoin = 'Round'
                $arrow.CustomEndCap = New-Object Drawing.Drawing2D.AdjustableArrowCap 4, 4
                $g.DrawLines($arrow, [Drawing.PointF[]]$points)
                $arrow.Dispose()
            }
        }
        $lp = New-RoundPath $label 7
        $g.FillPath($brush, $lp)
        $g.DrawPath((New-Object Drawing.Pen ([Drawing.Color]::White), 1.5), $lp)
        $g.DrawString($m.Text, $font, $white, $label.X + 8, $label.Y + 4)
    }
    $file = Join-Path $Out $name
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "  $name  $($crop.Width)x$($crop.Height)"
}
function Want([string]$name) { return $Only -eq '' -or $name -match $Only }

# ---- the Settings program ----------------------------------------------------------------------------------------------

$settingsExe = Join-Path $binCopy 'Settings.exe'
$cfg = Join-Path $Work 'settings'            # its four files (none: the defaults) and appearance.cfg
New-Item -ItemType Directory -Force $cfg | Out-Null
$shots = Join-Path $Work 'shots'
New-Item -ItemType Directory -Force $shots | Out-Null
function Settings-Shot([int]$page, [string]$tag, [scriptblock]$act = $null) {
    # The program says so in its bottom bar while any copy of the game runs (a test run included): the pictures show
    # the usual case, so they wait until none runs.
    for ($i = 0; (Get-Process -Name ShadowTheHedgehog -ErrorAction SilentlyContinue) -and $i -lt 360; $i++) {
        if ($i -eq 0) { "  waiting until no ShadowTheHedgehog.exe runs" }
        Start-Sleep -Seconds 5
    }
    Invoke-Snapshot $settingsExe @('--folder', $cfg, '--page', "$page", '--snapshot', (Join-Path $shots "settings-$tag.png")) (Join-Path $shots "settings-$tag.png") $act { Remove-Item (Join-Path $cfg 'display.cfg') -ErrorAction SilentlyContinue }
}
Write-Text (Join-Path $cfg 'appearance.cfg') "theme = dark`r`nzoom = 100`r`n"
# The zoom and theme lists: the two drop-down lists under the navigation list (their text is drawn by the program, so
# they are found by their place).
function Look-Lists($s) {
    $nav = Find-Class $s 'ListBox'
    return @($s.Controls | Where-Object { $_.Class -eq 'ComboBox' -and $_.X -lt $nav.Right -and $_.Y -gt $nav.Y } |
        Sort-Object Y | Select-Object -First 2 | ForEach-Object { New-Object Drawing.Rectangle $_.X, $_.Y, $_.W, $_.H })
}

$pageNames = @('display', 'frame-rate', 'sound', 'language', 'controller-buttons', 'controller-options', 'keyboard-mouse', 'saving')
if (Want 'settings') {
    "Settings"
    # 1: the window, its parts
    $s = Settings-Shot 0 'display'
    $nav = Find-Class $s 'ListBox'
    $look = Look-Lists $s
    $zoom = $look[0]; $theme = $look[1]
    $save = Find-Control $s 'Save'
    $restore = Find-Control $s 'Restore defaults'
    $close = Find-Control $s 'Close'
    $statusLines = Join-Rect (Find-Id $s 404) (Find-Id $s 405)
    # Labels sit on empty room only: the title bar right of the window's title, the room under the zoom and theme
    # lists, and a strip added under the window (Save-Marked -extend) for the bottom bar's labels.
    $below = $s.Frame.Bottom + 10
    Save-Marked $s 'settings/settings-1-window.png' $null @(
        @{ Box = (Grow $nav 3); Text = '1  The pages'; At = (New-Object Drawing.Point ($nav.Right + 24), ($s.Frame.Y + 1)) },
        @{ Box = (Grow (Join-Rect $zoom $theme) 3); Text = '2  Zoom and theme'; At = (New-Object Drawing.Point $nav.X, ($theme.Bottom + 40)) },
        @{ Box = (Grow $statusLines 3); Text = '3  What is going on'; At = (New-Object Drawing.Point $statusLines.X, $below) },
        @{ Box = (Grow (Join-Rect $restore $close) 3); Text = '4  Save when you are done'; At = (New-Object Drawing.Point $restore.X, $below) }) 50
    # 2..9: every page, its entry in the navigation list marked (the list's rows are 40 pixels at zoom 100 %:
    # main.cpp kNavItemH). The label is under the zoom and theme lists; its arrow runs up the empty gap between the
    # navigation and the page.
    for ($p = 0; $p -lt 8; $p++) {
        $s = if ($p -eq 0) { $s } else { Settings-Shot $p $pageNames[$p] }
        $nav = Find-Class $s 'ListBox'
        $look = Look-Lists $s
        $item = New-Object Drawing.Rectangle $nav.X, ($nav.Y + $p * 40), $nav.Width, 40
        $n = $p + 2
        $ly = $look[1].Bottom + 40
        $gapX = $nav.Right + 10
        $marks = @(@{ Box = (Grow $item 1); Text = 'Click here'; At = (New-Object Drawing.Point $nav.X, $ly)
                      Via = @((New-Object Drawing.Point $gapX, ($ly + 15)), (New-Object Drawing.Point $gapX, ($item.Y + 20))) })
        if ($p -eq 4) {
            # the list's first button, and the controller pictures (main.cpp build_pad_page(): 414 units right of the
            # page, 4 below its top (72), 668 x 578 at most, less when the window is not tall enough). Label 1 right of
            # the page's title, its arrow down the gap right of the list's buttons.
            $picture = New-Object Drawing.Rectangle ($s.Client.X + 224 + 414), ($s.Client.Y + 76), 668, ([Math]::Min(578, $s.Frame.Bottom - $s.Client.Y - 92))
            $first = Find-Id $s 100
            $columnGap = $first.Right + 16
            $marks = @(
                @{ Box = (Grow $first 3); Text = '1  Click a button in the list'; At = (New-Object Drawing.Point ($s.Client.X + 500), ($s.Client.Y + 22))
                   Via = @((New-Object Drawing.Point $columnGap, ($s.Client.Y + 37)), (New-Object Drawing.Point $columnGap, ($first.Y + $first.Height / 2))) },
                @{ Box = $picture; Text = '2  ...or the same button on the picture'; At = (New-Object Drawing.Point ($picture.X + 170), ($picture.Bottom - 48)); Arrow = $false })
        }
        # A short page is cut below its bottom bar (and its label); the controller and keyboard pages fill the window.
        $crop = $null
        if ($p -ne 4 -and $p -ne 6) {
            $crop = [Drawing.Rectangle]::FromLTRB($s.Frame.X, $s.Frame.Y, $s.Frame.Right, [Math]::Min($s.Frame.Bottom, [Math]::Max((Find-Id $s 405).Bottom + 20, $ly + 45)))
        }
        Save-Marked $s ("settings/settings-$n-" + $pageNames[$p] + '.png') $crop $marks
    }
    # 10: the keyboard page scrolled to its end: the mouse settings (label in the room under the navigation)
    $s = Settings-Shot 6 'keyboard-mouse-end' { param($h) [void][TutWin]::SendMessage($h, 0x0115, [IntPtr]7, [IntPtr]::Zero) }
    $speed = $s.Controls | Where-Object { $_.Class -eq 'Static' -and $_.Text -like 'Mouse speed, left*' } | Select-Object -First 1
    $walk = $s.Controls | Where-Object { $_.Class -eq 'Static' -and $_.Text -eq 'Walk speed' } | Select-Object -First 1
    $card = [Drawing.Rectangle]::FromLTRB($speed.X - 12, $speed.Y - 12, $s.Frame.Right - 40, $walk.Y + $walk.H + 16)
    $nav = Find-Class $s 'ListBox'
    Save-Marked $s 'settings/settings-10-mouse-settings.png' $null @(@{ Box = $card; Text = 'The mouse settings'; At = (New-Object Drawing.Point $nav.X, ($card.Y + 70)) })
    # 11: a controller button chosen (its row's button pressed): "Press a button..."
    $s = Settings-Shot 4 'pad-capture' { param($h) [void][TutWin]::SendMessage([TutWin]::GetDlgItem($h, 100), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
    $row = Find-Id $s 100
    $statusLines = Join-Rect (Find-Id $s 404) (Find-Id $s 405)
    $columnGap = $row.Right + 16
    Save-Marked $s 'settings/settings-11-press-a-button.png' $null @(
        @{ Box = (Grow $row 3); Text = '1  Waiting for your button'; At = (New-Object Drawing.Point ($s.Client.X + 500), ($s.Client.Y + 22))
           Via = @((New-Object Drawing.Point $columnGap, ($s.Client.Y + 37)), (New-Object Drawing.Point $columnGap, ($row.Y + $row.Height / 2))) },
        @{ Box = (Grow $statusLines 3); Text = '2  Press it on your controller now'; At = (New-Object Drawing.Point $statusLines.X, ($s.Frame.Bottom + 10)) }) 50
    # 12: a key box chosen on the keyboard page: "Press a key..." (Jump, its third box: the room right of it is empty)
    $s = Settings-Shot 6 'key-capture' { param($h) [void][TutWin]::SendMessage([TutWin]::GetDlgItem($h, 2000 + 9 * 3 + 2), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
    $box = Find-Id $s (2000 + 9 * 3 + 2)
    $nav = Find-Class $s 'ListBox'
    $look = Look-Lists $s
    $camera = @($s.Controls | Where-Object { $_.Class -eq 'ComboBox' -and $_.X -gt $nav.Right } | Sort-Object Y |
        ForEach-Object { New-Object Drawing.Rectangle $_.X, $_.Y, $_.W, $_.H })
    $top = [Drawing.Rectangle]::FromLTRB($s.Frame.X, $s.Frame.Y, $s.Frame.Right, ($box.Bottom + 330))
    $ly = $look[1].Bottom + 40
    $gapX = $nav.Right + 8
    $between = [int](($camera[0].Bottom + $camera[1].Y) / 2)   # the line between the Camera left and Camera right rows
    Save-Marked $s 'settings/settings-12-press-a-key.png' $top @(
        @{ Box = (Grow $box 3); Text = '1  Waiting for your key'; At = 'right' },
        @{ Box = (Grow (Join-Rect $camera[0] $camera[3]) 2); Text = '2  Camera mouse'; At = (New-Object Drawing.Point $nav.X, $ly)
           Via = @((New-Object Drawing.Point $gapX, ($ly + 15)), (New-Object Drawing.Point $gapX, $between)) })
    # 13: the Shadow theme at 125 %
    Write-Text (Join-Path $cfg 'appearance.cfg') "theme = shadow`r`nzoom = 125`r`n"
    $s = Settings-Shot 2 'shadow-theme'
    $look = Look-Lists $s
    Save-Marked $s 'settings/settings-13-shadow-theme.png' $null @(@{ Box = (Grow (Join-Rect $look[0] $look[1]) 3); Text = 'Zoom 125 %, theme Shadow'; At = (New-Object Drawing.Point $look[0].X, ($look[1].Bottom + 40)) })
    Write-Text (Join-Path $cfg 'appearance.cfg') "theme = dark`r`nzoom = 100`r`n"
    # 14, 15: a change (Smooth motion switched on: its switch clicked), then Save clicked. Writes the scratch display.cfg,
    # which is removed afterwards. Smooth motion is the eleventh setting (main.cpp define_fields(): eight on Display,
    # then V-Sync, Frame rate, Smooth motion): id 1000 + 10. Save is id 402 (kSave).
    $click = { param($h, [int]$id) [void][TutWin]::SendMessage([TutWin]::GetDlgItem($h, $id), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
    foreach ($step in @(@{ Tag = 'unsaved'; Save = $false }, @{ Tag = 'saved'; Save = $true })) {
        $doSave = $step.Save
        $s = Settings-Shot 1 $step.Tag { param($h) & $click $h 1010; if ($doSave) { Start-Sleep -Milliseconds 200; & $click $h 402 } }
        $look = Look-Lists $s
        $bottom = [Math]::Max((Find-Id $s 405).Bottom + 14, $look[1].Bottom + 14)
        $crop = [Drawing.Rectangle]::FromLTRB($s.Frame.X, $s.Frame.Y, $s.Frame.Right, $bottom)
        $switch = Find-Id $s 1010
        $statusLines = Join-Rect (Find-Id $s 404) (Find-Id $s 405)
        $below = $bottom + 10
        if ($step.Save) {
            $saveButton = Find-Control $s 'Save' 'Button'
            Save-Marked $s 'settings/settings-15-saved.png' $crop @(
                @{ Box = (Grow $statusLines 3); Text = '2  Saved'; At = (New-Object Drawing.Point $statusLines.X, $below) },
                @{ Box = (Grow $saveButton 3); Text = '1  Click Save'; At = (New-Object Drawing.Point $saveButton.X, $below) }) 50
            Remove-Item (Join-Path $cfg 'display.cfg')
        } else {
            # label 1 under the window, its arrow up the empty right margin to the switch
            $marginX = $s.Frame.Right - 16
            Save-Marked $s 'settings/settings-14-unsaved.png' $crop @(
                @{ Box = (Grow $switch 3); Text = '1  A change'; At = (New-Object Drawing.Point ($marginX - 170), $below)
                   Via = @((New-Object Drawing.Point $marginX, ($below + 15)), (New-Object Drawing.Point $marginX, ($switch.Y + $switch.Height / 2))) },
                @{ Box = (Grow $statusLines 3); Text = '2  Not saved yet'; At = (New-Object Drawing.Point $statusLines.X, $below) }) 50
        }
    }
}

# ---- the Mod Manager -----------------------------------------------------------------------------------------------------

$managerExe = Join-Path $binCopy 'ModManager.exe'
function Invoke-Manager([string[]]$arguments) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $managerExe
    $psi.Arguments = ((@('--mods-dir', $mods, '--game-data', $Game) + $arguments) | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(300000)) { $p.Kill(); throw "ModManager $arguments did not end" }
    if ($p.ExitCode -ne 0) { throw "ModManager $arguments failed ($($p.ExitCode)): $($o.Result) $($e.Result)" }
}
function Manager-Shot([string]$tag, [string]$select = '') {
    $png = Join-Path $shots "mods-$tag.png"
    $a = @('--mods-dir', $mods, '--game-data', $Game, '--snapshot', $png)
    if ($select) { $a += @('--select', $select) }
    Invoke-Snapshot $managerExe $a $png
}
# A list row's rectangle (0 = the top row) and a column's, from the list's place and the manager's own column widths
# (mod_manager.cpp layout(): name = the rest, then 60, 110, 170, 250 at zoom 100 %). The row height is the list's own
# (LVM_APPROXIMATEVIEWRECT, read while the window was open).
function Row-Rect($shot, [int]$row) {
    $list = Find-Class $shot 'SysListView32'; $head = Find-Class $shot 'SysHeader32'
    return New-Object Drawing.Rectangle ($list.X + 2), ($head.Bottom + $row * $shot.RowH), ($list.Width - 4), $shot.RowH
}
function Column-Rect($shot, [int]$column, [int]$firstRow, [int]$rows) {
    $list = Find-Class $shot 'SysListView32'; $head = Find-Class $shot 'SysHeader32'
    $widths = @([Math]::Max(140, $list.Width - 590 - 24), 60, 110, 170, 250)
    $x = $list.X + 2; for ($i = 0; $i -lt $column; $i++) { $x += $widths[$i] }
    $w = [Math]::Min($widths[$column], $list.Right - 2 - $x)
    return New-Object Drawing.Rectangle $x, ($head.Bottom + $firstRow * $shot.RowH), $w, ($rows * $shot.RowH)
}

# The example mods, made here from the game's own files and generated pictures; zipped like downloads.
function New-Download([string]$id, [string]$name, [string]$version, [string]$author, [string]$description, [hashtable]$copies, [string]$extraJson = '') {
    $src = Join-Path $Work "src\$id"
    New-Item -ItemType Directory -Force $src | Out-Null
    foreach ($to in $copies.Keys) {
        $target = Join-Path $src $to
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        $from = $copies[$to]
        if ($from -is [scriptblock]) { & $from $target } else { Copy-Item (Join-Path $Game "files\$from") $target }
    }
    $json = '{"schema":1,"id":"' + $id + '","name":"' + $name + '","version":"' + $version + '","author":"' + $author + '","description":"' + $description + '"' + $extraJson + '}'
    Write-Text (Join-Path $src 'mod.json') $json
    $zip = Join-Path $downloads "$id.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($src, $zip)
    return $zip
}
function New-Picture([string]$file, [int]$w, [int]$h, [string]$words, [Drawing.Color]$from, [Drawing.Color]$to) {
    $b = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($b)
    $brush = New-Object Drawing.Drawing2D.LinearGradientBrush (New-Object Drawing.Rectangle 0, 0, $w, $h), $from, $to, 45
    $g.FillRectangle($brush, 0, 0, $w, $h)
    if ($words) {
        $g.TextRenderingHint = 'AntiAlias'
        $f = New-Object Drawing.Font 'Segoe UI', ([int]($h / 7)), ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object Drawing.StringFormat; $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
        $g.DrawString($words, $f, [Drawing.Brushes]::White, (New-Object Drawing.RectangleF 0, 0, $w, $h), $fmt)
    }
    $g.Dispose(); $b.Save($file, [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}

if (Want 'mods') {
    "Mod Manager"
    Set-Look 'dark' 100
    $zips = @(
        (New-Download 'westopolis-music' 'Westopolis Music Swap' '1.0' 'Example Modder' 'Westopolis plays the song of another stage.' @{
            'files\sng_stg0100.adx' = 'sng_stg0200.adx'
            'docs\preview.png' = { param($t) New-Picture $t 640 360 'Example mod preview' ([Drawing.Color]::FromArgb(40, 60, 140)) ([Drawing.Color]::FromArgb(150, 30, 40)) } } ',"preview":"docs/preview.png"'),
        (New-Download 'other-westopolis-music' 'Other Westopolis Music' '2.0' 'Another Modder' 'Westopolis plays a third song.' @{ 'files\sng_stg0100.adx' = 'sng_stg0300.adx' }),
        (New-Download 'boss-music-choice' 'Boss Music Choice' '1.2' 'Example Modder' 'Another song for the battles, with a choice of song.' @{ 'files\sng_Battle1.adx' = 'sng_stg0400.adx'; 'options\song\second\files\sng_Battle1.adx' = 'sng_Battle2.adx' } ',"options":[{"id":"song","name":"Boss song","default":"original","choices":[{"id":"original","name":"A stage song","folder":""},{"id":"second","name":"The second battle song","folder":"options/song/second"}]}]'),
        (New-Download 'sample-textures' 'Sample Texture Pack' '1.0' 'Example Modder' 'A texture pack in the Dolphin format.' @{
            'textures\tex1_64x64_0123456789abcdef_5.png' = { param($t) New-Picture $t 64 64 '' ([Drawing.Color]::FromArgb(200, 40, 40)) ([Drawing.Color]::FromArgb(30, 30, 30)) } }))

    # 1: the empty window, where a download goes
    $s = Manager-Shot 'empty'
    $list = Find-Class $s 'SysListView32'
    Save-Marked $s 'mods/mods-1-drop-here.png' $null @(
        @{ Box = (Grow $list 2); Text = '1  Drop the downloaded file here'; At = (New-Object Drawing.Point ($list.X + 60), ($list.Y + 140)) ; Arrow = $false },
        @{ Box = (Grow (Find-Control $s 'Install...' 'Button') 3); Text = '2  Or click Install... and pick it'; At = (New-Object Drawing.Point ($s.Frame.X + 260), ($s.Frame.Y + 1)) })

    # the downloads installed, as a drop or Install... does (the same install, from the manager's command line)
    foreach ($z in $zips) { Invoke-Manager @('install', $z) }

    # 2: installed, all off; tick the first one
    $s = Manager-Shot 'installed' 'westopolis-music'
    $row0 = Row-Rect $s 0
    $tick = New-Object Drawing.Rectangle ($row0.X + 2), $row0.Y, ($s.RowH + 2), $row0.Height
    Save-Marked $s 'mods/mods-2-tick.png' $null @(
        @{ Box = (Grow $tick 2); Text = '1  Tick the box to switch the mod on'; At = (New-Object Drawing.Point ($row0.X + 30), ($row0.Bottom + 130))
           Via = @((New-Object Drawing.Point ($row0.X - 8), ($row0.Bottom + 145)), (New-Object Drawing.Point ($row0.X - 8), ($row0.Y + $row0.Height / 2))) },
        @{ Box = (Grow (Column-Rect $s 4 0 4) 1); Text = '2  Off: not ticked yet'; At = 'below' })

    # 3: two mods that change the same file, both on: the higher one wins
    Invoke-Manager @('enable', 'westopolis-music'); Invoke-Manager @('enable', 'other-westopolis-music')
    $s = Manager-Shot 'conflict' 'westopolis-music'
    $details = Find-Id $s 109
    Save-Marked $s 'mods/mods-3-conflict.png' $null @(
        @{ Box = (Grow (Column-Rect $s 4 0 2) 1); Text = '1  Who wins, who loses'; At = 'below' },
        @{ Box = (Grow (Join-Rect (Find-Control $s 'Move up' 'Button') (Find-Control $s 'Move down' 'Button')) 3); Text = '2  Change the order'; At = (New-Object Drawing.Point ((Find-Control $s 'Move up' 'Button').X + 40), ($s.Frame.Y + 1)) },
        @{ Box = (Grow $details 1); Text = '3  The conflicts, in words'; At = 'left' })

    # 4: a mod with an option
    Invoke-Manager @('enable', 'boss-music-choice')
    $s = Manager-Shot 'options' 'boss-music-choice'
    $label = Find-Control $s 'Boss song' 'Static'
    $combo = $s.Controls | Where-Object { $_.Class -eq 'ComboBox' -and [Math]::Abs($_.Y - $label.Y) -lt 20 } | Select-Object -First 1
    $combo = New-Object Drawing.Rectangle $combo.X, $combo.Y, $combo.W, $combo.H
    Save-Marked $s 'mods/mods-4-option.png' $null @(@{ Box = (Grow (Join-Rect $label $combo) 3); Text = 'Choose here'; At = 'left' })

    # 5: a texture pack
    Invoke-Manager @('enable', 'sample-textures')
    $s = Manager-Shot 'textures' 'sample-textures'
    Save-Marked $s 'mods/mods-5-texture-pack.png' $null @(@{ Box = (Grow (Column-Rect $s 3 3 1) 1); Text = 'A texture pack: textures, not files'; At = 'below' })

    # 6: all mods switched off at once
    Invoke-Manager @('master', 'off')
    $s = Manager-Shot 'master-off' 'westopolis-music'
    Save-Marked $s 'mods/mods-6-load-mods-off.png' $null @(
        @{ Box = (Grow (Find-Control $s 'Load mods' 'Button') 3); Text = '1  Untick Load mods'; At = (New-Object Drawing.Point ((Find-Control $s 'Load mods' 'Button').X - 230), ($s.Frame.Y + 1)) },
        @{ Box = (Grow (Column-Rect $s 4 0 4) 1); Text = '2  The ticks stay, nothing loads'; At = 'below' })
    Invoke-Manager @('master', 'on')

    # 7: Play, Profiles, Remove
    $s = Manager-Shot 'buttons' 'other-westopolis-music'
    $list = Find-Class $s 'SysListView32'
    $profiles = $s.Controls | Where-Object { $_.Class -eq 'Button' -and $_.Text -like 'Profiles*' } | ForEach-Object { New-Object Drawing.Rectangle $_.X, $_.Y, $_.W, $_.H } | Select-Object -First 1
    $play = Find-Control $s 'Play' 'Button'
    Save-Marked $s 'mods/mods-7-buttons.png' $null @(
        @{ Box = (Grow (Find-Control $s 'Remove' 'Button') 3); Text = '1  Remove the selected mod'; At = (New-Object Drawing.Point ($s.Frame.X + 250), ($s.Frame.Y + 1)) },
        @{ Box = (Grow $profiles 3); Text = '2  Save and load sets of mods'; At = (New-Object Drawing.Point ($s.Frame.X + 515), ($s.Frame.Y + 1)) },
        @{ Box = (Grow $play 3); Text = '3  Start the game'; At = (New-Object Drawing.Point ($s.Frame.X + 820), ($s.Frame.Y + 1)) })
}
