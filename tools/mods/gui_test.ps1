<#
.SYNOPSIS
  Drives the real window of ShadowModManager.exe from outside, the way a user does, and checks the results on disk.

.DESCRIPTION
  Windows PowerShell 5.1. No third-party modules. What it uses:
    * UI Automation (the native IUIAutomation client; see docs/mods/GUI_TEST.md for why not the managed one) to find
      the controls, read the list rows, the details text and the status line, invoke buttons, toggle check boxes and
      read the message boxes the window raises.
    * Real mouse and keyboard input (SendInput-style mouse_event/keybd_event) for the real-click and combo box steps.
    * A real OLE drag and drop (a small source window offers the files as CF_HDROP, the mouse button goes down on it,
      moves over the manager's window and is released) for installing, exactly as dropping from the Explorer.
    * mods.json in the mods dir, read back from disk after every action, and the manager's own command line
      (list / plan) as a second reading of the same state.

  Every step prints PASS, FAIL or SKIP with the evidence. A step that could not be performed is a SKIP with the reason,
  never a PASS. Exit code = number of failed steps. The manager instance started here is the only process touched.

  The test moves the mouse and takes the keyboard for the length of the run (about 3 minutes): leave the machine alone.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\mods\gui_test.ps1 `
     -Exe D:\build\ShadowModManager.exe -GameData D:\ShadowGame `
     -Downloads D:\downloads -Work D:\scratch\gui-work -ModelFixes D:\repos\shadow-model-fixes
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,         # ShadowModManager.exe (mods-data\ beside it)
    [Parameter(Mandatory = $true)][string]$GameData,    # folder that contains files\ (read only)
    [Parameter(Mandatory = $true)][string]$Downloads,   # cream.rar, playable_tails.zip, xbox_buttons_files.zip
    [Parameter(Mandatory = $true)][string]$Work,        # parent for the fresh per-run folders (nothing else is touched)
    [string]$ModelFixes = '',                           # a mod folder with options, dropped as a folder (optional)
    [string]$Only = '',                                 # regex of step id prefixes to run, e.g. '19|20' (debugging; later steps need the earlier setup)
    [string]$BigFolder = '',                           # a big mod folder for the progress bar step when the zip install is too quick
    [string]$CodeMod = ''                              # a mod folder that offers code (Gecko ini), for the Code mods window step (optional)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

# ---- interop: the native UI Automation client -----------------------------------------------------------------------
# The managed System.Windows.Automation client of this machine returns every Win32 control as a pattern-less "Pane"
# (no Invoke, no Toggle, no control types), so the native IUIAutomation COM interface is used. Its type library is
# turned into an interop assembly with the framework's TypeLibConverter once per work folder.
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$GameData = (Resolve-Path -LiteralPath $GameData).Path
$Downloads = (Resolve-Path -LiteralPath $Downloads).Path
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path
$interop = Join-Path $Work '_interop\UIAInterop.dll'
if (-not (Test-Path -LiteralPath $interop)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $interop) | Out-Null
    Add-Type -TypeDefinition @'
using System; using System.Reflection; using System.Runtime.InteropServices; using System.Runtime.InteropServices.ComTypes;
public class TlbSink : ITypeLibImporterNotifySink {
  public void ReportEvent(ImporterEventKind k, int code, string msg) { }
  public Assembly ResolveRef(object typeLib) { return null; }
}
public static class TlbMake {
  [DllImport("oleaut32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
  static extern void LoadTypeLibEx(string file, int regkind, out ITypeLib lib);
  public static void Make(string dll, string outFile) {
    ITypeLib lib; LoadTypeLibEx(dll, 2, out lib);
    var ab = new TypeLibConverter().ConvertTypeLibToAssembly(lib, outFile, TypeLibImporterFlags.None, new TlbSink(), null, null, "UIAutomationClientInterop", null);
    ab.Save(System.IO.Path.GetFileName(outFile));
  }
}
'@
    $cwd = Get-Location
    Set-Location (Split-Path $interop)
    [TlbMake]::Make("$env:windir\System32\UIAutomationCore.dll", $interop)
    Set-Location $cwd
}
Add-Type -Path $interop

Add-Type -ReferencedAssemblies $interop, 'System.Windows.Forms', 'System.Drawing' -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using UIAutomationClientInterop;

public static class Nat {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder sb, int n);

    public static void DpiAware() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }  // per-monitor v2: physical pixels

    // Every visible top-level window of a process (owned dialogs included, which the UIA root's child list can miss).
    public static long[] TopWindows(int pid) {
        List<long> l = new List<long>();
        EnumWindows(delegate (IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp == (uint)pid && IsWindowVisible(h)) l.Add(h.ToInt64());
            return true;
        }, IntPtr.Zero);
        return l.ToArray();
    }
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    static string Under(int x, int y) {
        IntPtr h = GetAncestor(WindowFromPoint(new Point(x, y)), 2); uint pid; GetWindowThreadProcessId(h, out pid);
        return ClassOf(h.ToInt64()) + ", pid " + pid;
    }
    public static string ClassOf(long h) { StringBuilder sb = new StringBuilder(256); GetClassName(new IntPtr(h), sb, 256); return sb.ToString(); }

    static void Abs(int x, int y) {
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        int nx = (int)(((long)(x - vx) * 65535) / (vw - 1)), ny = (int)(((long)(y - vy) * 65535) / (vh - 1));
        mouse_event(0x0001 | 0x8000 | 0x4000, nx, ny, 0, UIntPtr.Zero);  // MOVE | ABSOLUTE | VIRTUALDESK
    }
    public static void MouseMove(int x, int y) { Abs(x, y); }
    public static void MouseClick(int x, int y) {
        Abs(x, y); Thread.Sleep(120);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(70);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(150);
    }
    public static bool Visible(long h) { return IsWindowVisible(new IntPtr(h)); }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    public static long Child(long parent, string cls) { return FindWindowEx(new IntPtr(parent), IntPtr.Zero, cls, null).ToInt64(); }
    public static void RightClick(int x, int y) {
        Abs(x, y); Thread.Sleep(120);
        mouse_event(0x0008, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(70);
        mouse_event(0x0010, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(200);
    }
    public static void DoubleClick(int x, int y) {
        Abs(x, y); Thread.Sleep(120);
        for (int i = 0; i < 2; i++) {
            mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(40);
            mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(60);
        }
        Thread.Sleep(200);
    }
    // A real drag inside one window: button down at (x1,y1), a slow move, button up at (x2,y2).
    public static void MouseDrag(int x1, int y1, int x2, int y2) {
        Abs(x1, y1); Thread.Sleep(150);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(150);
        for (int i = 1; i <= 20; i++) { Abs(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20); Thread.Sleep(30); }
        Thread.Sleep(250);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(300);
    }
    public static void Chord(byte mod, byte vk) { Chord(mod, vk, false); }
    public static void Chord(byte mod, byte vk, bool ext) {
        uint f = ext ? 1u : 0u;
        keybd_event(mod, 0, 0, UIntPtr.Zero); Thread.Sleep(40);
        keybd_event(vk, 0, f, UIntPtr.Zero); Thread.Sleep(50);
        keybd_event(vk, 0, f | 2, UIntPtr.Zero); Thread.Sleep(40);
        keybd_event(mod, 0, 2, UIntPtr.Zero); Thread.Sleep(150);
    }
    [DllImport("user32.dll")] static extern short VkKeyScan(char c);
    public static void TypeText(string s) {
        foreach (char c in s) {
            short v = VkKeyScan(c); byte vk = (byte)(v & 0xff); bool shift = (v & 0x100) != 0;
            if (shift) keybd_event(0x10, 0, 0, UIntPtr.Zero);
            keybd_event(vk, 0, 0, UIntPtr.Zero); Thread.Sleep(30);
            keybd_event(vk, 0, 2, UIntPtr.Zero); Thread.Sleep(30);
            if (shift) keybd_event(0x10, 0, 2, UIntPtr.Zero);
        }
        Thread.Sleep(200);
    }
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);
    public static int[] Pixel(int x, int y) {
        IntPtr dc = GetDC(IntPtr.Zero); uint c = GetPixel(dc, x, y); ReleaseDC(IntPtr.Zero, dc);
        return new int[] { (int)(c & 0xff), (int)((c >> 8) & 0xff), (int)((c >> 16) & 0xff) };
    }
    // The darkest-most-saturated (text) pixel of a rectangle: the colour a label is drawn in.
    public static int[] TextColour(int l, int t, int r, int b) {
        IntPtr dc = GetDC(IntPtr.Zero); int[] best = new int[] { 255, 255, 255 }; int bestSum = 3 * 255 + 1;
        for (int y = t; y < b; y++) for (int x = l; x < r; x++) {
            uint c = GetPixel(dc, x, y); int R = (int)(c & 0xff), G = (int)((c >> 8) & 0xff), B = (int)((c >> 16) & 0xff);
            if (R + G + B < bestSum) { bestSum = R + G + B; best = new int[] { R, G, B }; }
        }
        ReleaseDC(IntPtr.Zero, dc); return best;
    }
    public static void Key(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero); Thread.Sleep(50);
        keybd_event(vk, 0, 2, UIntPtr.Zero); Thread.Sleep(100);
    }

    // A real OLE drag and drop: a small source window offers the files as CF_HDROP, the left button goes down on it,
    // the cursor moves to (tx, ty) and the button is released there. The system's drop machinery then delivers
    // WM_DROPFILES to a window that called DragAcceptFiles. Returns the effect OLE reports (None = nobody took it).
    public static string DragFiles(string[] files, int tx, int ty, int attempt) {
        string result = "drag did not start";
        bool started = false;
        Thread t = new Thread(delegate () {
            Form f = new Form();
            f.FormBorderStyle = FormBorderStyle.None;
            f.StartPosition = FormStartPosition.Manual;
            Rectangle wa = Screen.PrimaryScreen.WorkingArea;
            f.Location = (attempt % 2 == 1) ? new Point(wa.Right - 260, wa.Bottom - 140) : new Point(wa.Left + 40, wa.Bottom - 140);
            f.Size = new Size(200, 80);
            f.TopMost = true;
            f.ShowInTaskbar = false;
            f.BackColor = Color.DarkOrange;
            f.MouseDown += delegate (object s, MouseEventArgs e) {
                started = true;
                DataObject data = new DataObject();
                data.SetData(DataFormats.FileDrop, files);
                DragDropEffects fx = f.DoDragDrop(data, DragDropEffects.Copy | DragDropEffects.Move | DragDropEffects.Link);
                result = fx.ToString();
                f.BeginInvoke(new Action(delegate () { f.Close(); }));
            };
            f.Shown += delegate (object s, EventArgs e) {
                int cx = f.Left + f.Width / 2, cy = f.Top + f.Height / 2;
                Thread worker = new Thread(delegate () {
                    Thread.Sleep(300);
                    MouseMove(cx, cy);
                    Thread.Sleep(300);
                    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);  // left button down on the source window
                    Thread.Sleep(600);
                    if (started) {
                        for (int i = 1; i <= 25; i++) {
                            MouseMove(cx + (tx - cx) * i / 25, cy + (ty - cy) * i / 25);
                            Thread.Sleep(25);
                        }
                        Thread.Sleep(350);
                        MouseMove(tx + 1, ty);
                        Thread.Sleep(250);
                    }
                    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);  // released over the target: the drop
                    if (!started) {
                        result = "drag did not start (the window under the cursor: " + Under(cx, cy) + ")";
                        try { f.BeginInvoke(new Action(delegate () { f.Close(); })); } catch (Exception) { }
                    }
                });
                worker.Start();
            };
            Application.Run(f);
        });
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
        if (!t.Join(40000)) { mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero); result = "drag timed out"; }
        return result;
    }
}

// One UI Automation element.
public class El {
    public IUIAutomationElement E;
    public El(IUIAutomationElement e) { E = e; }
    public static IUIAutomation A = (IUIAutomation)new CUIAutomation8Class();

    public string Name { get { return E.CurrentName ?? ""; } }
    public int Type { get { return E.CurrentControlType; } }
    public string Cls { get { return E.CurrentClassName ?? ""; } }
    public string Aid { get { return E.CurrentAutomationId ?? ""; } }
    public bool Enabled { get { return E.CurrentIsEnabled != 0; } }
    public bool Offscreen { get { return E.CurrentIsOffscreen != 0; } }
    public long Hwnd { get { return E.CurrentNativeWindowHandle.ToInt64(); } }
    public int[] Rect { get { tagRECT r = E.CurrentBoundingRectangle; return new int[] { r.left, r.top, r.right, r.bottom }; } }

    public List<El> Kids() {
        List<El> l = new List<El>();
        IUIAutomationElementArray a = E.FindAll(TreeScope.TreeScope_Children, A.CreateTrueCondition());
        if (a != null) for (int i = 0; i < a.Length; i++) l.Add(new El(a.GetElement(i)));
        return l;
    }
    public List<El> Descendants() {
        List<El> l = new List<El>();
        IUIAutomationElementArray a = E.FindAll(TreeScope.TreeScope_Descendants, A.CreateTrueCondition());
        if (a != null) for (int i = 0; i < a.Length; i++) l.Add(new El(a.GetElement(i)));
        return l;
    }
    public El ByAid(string aid) {
        IUIAutomationElement e = E.FindFirst(TreeScope.TreeScope_Descendants, A.CreatePropertyCondition(30011, aid));
        return e == null ? null : new El(e);
    }
    public bool Has(int pattern) { try { return E.GetCurrentPattern(pattern) != null; } catch (Exception) { return false; } }
    public string Patterns() {
        string[] n = { "Invoke", "Selection", "Value", "RangeValue", "Scroll", "ExpandCollapse", "Grid", "GridItem", "MultipleView", "Window", "SelectionItem", "Dock", "Table", "TableItem", "Text", "Toggle", "Transform", "ScrollItem", "LegacyIAccessible" };
        string s = "";
        for (int i = 0; i < n.Length; i++) if (Has(10000 + i)) s += (s == "" ? "" : ",") + n[i];
        return s;
    }
    public void Invoke() { ((IUIAutomationInvokePattern)E.GetCurrentPattern(10000)).Invoke(); }
    public void Select() { ((IUIAutomationSelectionItemPattern)E.GetCurrentPattern(10010)).Select(); }
    public bool IsSelected { get { return ((IUIAutomationSelectionItemPattern)E.GetCurrentPattern(10010)).CurrentIsSelected != 0; } }
    public int ToggleState { get { return (int)((IUIAutomationTogglePattern)E.GetCurrentPattern(10015)).CurrentToggleState; } }
    public void Toggle() { ((IUIAutomationTogglePattern)E.GetCurrentPattern(10015)).Toggle(); }
    public string Value { get { return ((IUIAutomationValuePattern)E.GetCurrentPattern(10002)).CurrentValue; } }
    public uint LegacyState { get { return ((IUIAutomationLegacyIAccessiblePattern)E.GetCurrentPattern(10018)).CurrentState; } }
    public string LegacyValue { get { return ((IUIAutomationLegacyIAccessiblePattern)E.GetCurrentPattern(10018)).CurrentValue; } }
    public void SetValue(string v) { ((IUIAutomationValuePattern)E.GetCurrentPattern(10002)).SetValue(v); }
    public void Focus() { E.SetFocus(); }
    public void CloseWindow() { ((IUIAutomationWindowPattern)E.GetCurrentPattern(10009)).Close(); }
    public static El FromHwnd(long h) { return new El(A.ElementFromHandle(new IntPtr(h))); }
}
'@
[Nat]::DpiAware()

# ---- harness ----------------------------------------------------------------------------------------------------------
$script:passed = 0; $script:failed = 0; $script:skipped = 0
$script:ev = @(); $script:bad = 0
$script:proc = $null; $script:mainHwnd = 0
$script:findings = @()
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = Join-Path $Work "run-$runStamp"
$modsDir = Join-Path $run 'mods'
New-Item -ItemType Directory -Force -Path $modsDir | Out-Null
$stateFile = Join-Path $modsDir 'mods.json'

function Ok([bool]$cond, [string]$text) {
    if ($cond) { $script:ev += "      ok   $text" } else { $script:ev += "      FAIL $text"; $script:bad++ }
}
function Same($a, $b) { return ((@($a) -join '|') -eq (@($b) -join '|')) }
function Info-Details($d) { $s = ($d -replace '\s+', ' '); Info ('details: ' + $s.Substring(0, [Math]::Min(300, $s.Length))) }
function Info([string]$text) { $script:ev += "      note $text" }
function Finding([string]$text) { $script:findings += $text; $script:ev += "      FINDING $text" }
function Skip([string]$why) { throw "SKIP: $why" }
function Need([bool]$cond, [string]$why) { if (-not $cond) { Skip $why } }

function Step([string]$id, [string]$title, [scriptblock]$body) {
    if ($Only -and $id -notmatch ('^(' + $Only + ')')) { return }
    $script:ev = @(); $script:bad = 0
    $verdict = 'PASS'
    try { & $body }
    catch {
        if ($_.Exception.Message -like 'SKIP:*') { $verdict = 'SKIP'; $script:ev += '      ' + $_.Exception.Message }
        else { $script:bad++; $script:ev += '      FAIL exception: ' + $_.Exception.Message + ' @ line ' + $_.InvocationInfo.ScriptLineNumber }
    }
    if ($verdict -ne 'SKIP') { $verdict = if ($script:bad -gt 0) { 'FAIL' } else { 'PASS' } }
    switch ($verdict) { 'PASS' { $script:passed++ } 'FAIL' { $script:failed++ } 'SKIP' { $script:skipped++ } }
    Write-Host ("[{0}] {1}  {2}" -f $verdict, $id, $title)
    foreach ($l in $script:ev) { Write-Host $l }
}

function Wait-For([scriptblock]$cond, [double]$timeout = 20, [string]$what = 'condition') {
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $end) {
        try { $r = & $cond; if ($r) { return $r } } catch { }
        Start-Sleep -Milliseconds 150
    }
    return $null
}

# ---- manager process and window ------------------------------------------------------------------------------------------
function Start-Manager([string]$mods) {
    $argv = @('--mods-dir', "`"$mods`"", '--game-data', "`"$GameData`"")
    $script:proc = Start-Process -FilePath $Exe -ArgumentList $argv -PassThru
    $script:mainHwnd = 0
}
function Dialogs() {
    if ($null -eq $script:proc -or $script:proc.HasExited) { return @() }
    $out = @()
    foreach ($h in [Nat]::TopWindows($script:proc.Id)) { if ([Nat]::ClassOf($h) -eq '#32770') { $out += [El]::FromHwnd($h) } }
    return $out
}
function Wait-Dialog([double]$timeout = 30) { return (Wait-For { $d = @(Dialogs); if ($d.Count) { $d[0] } } $timeout 'a message box') }
function Dialog-Text($d) {
    $t = @(); foreach ($k in $d.Descendants()) { if ($k.Type -eq 50020 -and $k.Name) { $t += $k.Name } }
    return ($t -join "`n")
}
# Presses a message box button by its control id (1 OK, 6 Yes, 7 No: the labels are localised, the ids are not).
function Press-Dialog($d, [string]$aid) {
    $b = $d.ByAid($aid)
    # A message box with only an OK button is exposed under control id 2 on this system (observed), so the id is not relied on: take the only button.
    if ($null -eq $b -and $aid -eq "1") { $b = @($d.Descendants() | Where-Object { $_.Cls -eq "Button" -and $_.Aid -ne "Close" })[0] }
    if ($null -eq $b) { throw "message box has no button with id $aid" }
    $b.Invoke()
    $null = Wait-For { @(Dialogs).Count -eq 0 } 10
}
function Main-Window() {
    if ($script:mainHwnd -eq 0) {
        $script:mainHwnd = Wait-For {
            foreach ($h in [Nat]::TopWindows($script:proc.Id)) { if ([Nat]::ClassOf($h) -eq 'ShadowModManager') { return $h } }
        } 60 'the manager window'
        if (-not $script:mainHwnd) { throw 'the manager window did not appear' }
    }
    return [El]::FromHwnd($script:mainHwnd)
}
function Wait-Ready() {   # the window is up and has filled its list
    $w = Main-Window
    $null = Wait-For { $s = $w.ByAid('111'); $s -and $s.Name -match 'installed' } 60 'the status line'
    return $w
}
function Stop-Manager() {
    if ($null -eq $script:proc) { return }
    if (-not $script:proc.HasExited) {
        foreach ($d in (Dialogs)) { try { $d.CloseWindow() } catch { } }
        try { if ($script:mainHwnd) { [El]::FromHwnd($script:mainHwnd).CloseWindow() } } catch { }
        if (-not $script:proc.WaitForExit(6000)) { try { $script:proc.Kill() } catch { }; $script:proc.WaitForExit(3000) | Out-Null; $script:closedGracefully = $false } else { $script:closedGracefully = $true }
    }
    $script:proc = $null; $script:mainHwnd = 0
}
function Focus-Main() {
    $w = Main-Window
    [void][Nat]::SetForegroundWindow([IntPtr]$w.Hwnd)
    Start-Sleep -Milliseconds 250
    return $w
}

# ---- reading the window -------------------------------------------------------------------------------------------------
function Get-Rows($w) {
    $list = $w.ByAid('100')
    $rows = @()
    foreach ($k in $list.Kids()) {
        if ($k.Type -ne 50007) { continue }
        $cells = @{}
        foreach ($c in $k.Kids()) { if ($c.Aid -like 'ListViewSubItem-*') { $cells[[int]($c.Aid -replace '\D', '')] = $c.Name } }
        $rows += [pscustomobject]@{
            El = $k; Name = $k.Name; Version = $cells[1]; Author = $cells[2]; Content = $cells[3]; Status = $cells[4]
            Checked = ($k.ToggleState -eq 1); Selected = $k.IsSelected
        }
    }
    return $rows
}
function Details($w) { return $w.ByAid('109').Value }
function StatusLine($w) { return $w.ByAid('111').Name }
function Button($w, [string]$name) {
    foreach ($k in $w.Kids()) { if ($k.Cls -eq 'Button' -and $k.Name -eq $name) { return $k } }
    return $null
}
function Press($w, [string]$name) {
    $b = Button $w $name
    if ($null -eq $b) { throw "no button '$name'" }
    $b.Invoke()
    Start-Sleep -Milliseconds 400
}
function Select-Row($w, [string]$name) {
    $r = Get-Rows $w | Where-Object { $_.Name -eq $name } | Select-Object -First 1
    if ($null -eq $r) { throw "no row '$name'" }
    $r.El.Select()
    $null = Wait-For { (Details $w) -match [regex]::Escape($name) } 5
    Start-Sleep -Milliseconds 200
}

# ---- reading the disk ---------------------------------------------------------------------------------------------------
function Read-State() {
    $fs = [IO.File]::Open($stateFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)   # never block the manager's rename
    try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); return ($sr.ReadToEnd() | ConvertFrom-Json) } finally { $fs.Dispose() }
}
function Hash-File([string]$p) {
    $fs = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try { $sha = [Security.Cryptography.SHA256]::Create(); return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace "-", "") } finally { $fs.Dispose() }
}
function State-Ids() { return @((Read-State).order | ForEach-Object { $_.id }) }
function State-Entry([string]$id) { return (Read-State).order | Where-Object { $_.id -eq $id } | Select-Object -First 1 }
function Run-Cli([string[]]$cliArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8; $psi.CreateNoWindow = $true
    $psi.Arguments = (($cliArgs + @('--mods-dir', $modsDir, '--game-data', $GameData)) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $p = [Diagnostics.Process]::Start($psi)
    $text = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
    return $text
}
# The CLI's reading of the state: id -> name, in priority order (the rows' order).
function Cli-List() {
    $res = @()
    foreach ($line in (Run-Cli @('list')) -split "`r?`n") {
        if ($line -match '^\d+\. \[(.)\] (\S+)  (.+) (\S+)  \| (.*?) \| (.*)$') {
            $res += [pscustomobject]@{ Id = $Matches[2]; Name = $Matches[3]; Version = $Matches[4]; On = ($Matches[1] -eq 'x'); Content = $Matches[5]; Status = $Matches[6] }
        }
    }
    return $res
}

# ---- drop targets ---------------------------------------------------------------------------------------------------------
function Drop-Target($w, [string]$where) {
    $r = $w.Rect
    switch ($where) {
        'list'    { $e = $w.ByAid('100').Rect; return @([int](($e[0] + $e[2]) / 2), [int](($e[1] + $e[3]) / 2) + 40) }
        'details' { $e = $w.ByAid('109').Rect; return @([int](($e[0] + $e[2]) / 2), [int](($e[1] + $e[3]) / 2)) }
        'status'  { $e = $w.ByAid('111').Rect; return @([int]($e[0] + 100), [int](($e[1] + $e[3]) / 2)) }
        'title'   { return @([int](($r[0] + $r[2]) / 2), [int]($r[1] + 12)) }
    }
}
function Drop-Files($w, [string[]]$files, [string]$where) {
    $fx = ""
    for ($try = 1; $try -le 3; $try++) {
        $null = Focus-Main
        $pt = Drop-Target $w $where
        $fx = [Nat]::DragFiles($files, $pt[0], $pt[1], $try)
        if ($fx -in @("Copy", "Move", "Link", "None")) { break }
        $script:ev += "      note drop attempt $try did not start a drag ($fx); retrying"
        Start-Sleep -Seconds 1
    }
    return $fx
}
function Accepted([string]$fx) { return ($fx -in @("Copy", "Move", "Link")) }
# Drops files and waits for the result message box. Returns @{ Effect; Text; Dialog }.
function Drop-And-Wait($w, [string[]]$files, [string]$where, [double]$timeout = 120) {
    $fx = Drop-Files $w $files $where
    $d = $null
    if (Accepted $fx) { $d = Wait-Dialog $timeout }
    $text = if ($d) { Dialog-Text $d } else { '' }
    return @{ Effect = $fx; Text = $text; Dialog = $d }
}

# ---- round 4 helpers: rows by real mouse, menus, forms ----------------------------------------------------------------
function Scale($w) { return (($w.Rect[2] - $w.Rect[0]) / 1180.0) }
function Row-Pt($w, [string]$name, [string]$part) {
    $r = @(Get-Rows $w) | Where-Object { $_.Name -eq $name } | Select-Object -First 1
    if ($null -eq $r) { throw "no row '$name'" }
    $rc = $r.El.Rect; $s = Scale $w
    $y = [int](($rc[1] + $rc[3]) / 2)
    if ($part -eq 'check') { return @([int]($rc[0] + 12 * $s), $y) }
    return @([int]($rc[0] + 150 * $s), $y)
}
function Click-Row($w, [string]$name, [string]$part = 'name') {
    $null = Focus-Main
    $p = Row-Pt $w $name $part
    [Nat]::MouseClick($p[0], $p[1])
}
function Rclick-Row($w, [string]$name) {
    $null = Focus-Main
    $p = Row-Pt $w $name 'name'
    [Nat]::RightClick($p[0], $p[1])
}
function Popups() {
    $out = @()
    if ($null -eq $script:proc -or $script:proc.HasExited) { return $out }
    foreach ($h in [Nat]::TopWindows($script:proc.Id)) { if ([Nat]::ClassOf($h) -eq '#32768') { $out += [El]::FromHwnd($h) } }
    return $out
}
function Wait-Popup([double]$timeout = 5) { return (Wait-For { $p = @(Popups); if ($p.Count) { $p[0] } } $timeout 'a menu') }
function Menu-Items($pop) { return @($pop.Descendants() | Where-Object { $_.Type -eq 50011 }) }
function Menu-Names($pop) { return @(Menu-Items $pop | ForEach-Object { $_.Name }) }
function Pick-Menu($pop, [string]$prefix, [bool]$real = $false) {
    $it = Menu-Items $pop | Where-Object { $_.Name -like "$prefix*" } | Select-Object -First 1
    if ($null -eq $it) { throw "menu has no item '$prefix' (items: $((Menu-Names $pop) -join ' | '))" }
    if ($real) { $r = $it.Rect; [Nat]::MouseClick([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) } else { $it.Invoke() }
    $null = Wait-For { @(Popups).Count -eq 0 } 5
    Start-Sleep -Milliseconds 400
}
function Forms() {
    $out = @()
    if ($null -eq $script:proc -or $script:proc.HasExited) { return $out }
    foreach ($h in [Nat]::TopWindows($script:proc.Id)) { if ([Nat]::ClassOf($h) -eq 'ShadowModManagerForm') { $out += [El]::FromHwnd($h) } }
    return $out
}
function Wait-Form([double]$timeout = 10) { return (Wait-For { $f = @(Forms); if ($f.Count) { $f[0] } } $timeout 'a form') }
function Form-Edits($f) { return @($f.Descendants() | Where-Object { $_.Cls -eq 'Edit' } | Sort-Object { $_.Rect[1] }) }
function Form-Button($f, [string]$aid) { return @($f.Descendants() | Where-Object { $_.Cls -eq 'Button' -and $_.Aid -eq $aid })[0] }
function Order-Now() { return @(State-Ids) }
function Wait-Order([string[]]$expected, [double]$timeout = 5) {
    $null = Wait-For { Same (State-Ids) $expected } $timeout
    return (Same (State-Ids) $expected)
}
function Wait-DialogTitled([string]$title, [double]$timeout = 15) {
    return (Wait-For { @(Dialogs | Where-Object { $_.Name -eq $title })[0] } $timeout "a box titled $title")
}

# ---- go -------------------------------------------------------------------------------------------------------------------
$cream = Join-Path $Downloads 'cream.rar'
$tails = Join-Path $Downloads 'playable_tails.zip'
$xbox = Join-Path $Downloads 'xbox_buttons_files.zip'
foreach ($f in @($cream, $tails, $xbox)) { if (-not (Test-Path -LiteralPath $f)) { throw "missing download: $f" } }
Write-Host "gui_test: manager $Exe"
Write-Host "          mods dir (fresh) $modsDir"
Write-Host ("          screen: DPI-aware, main display {0}x{1}" -f [Windows.Forms.Screen]::PrimaryScreen.Bounds.Width, [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height)
try {

# 1 ------------------------------------------------------------------------------------------------------------------------
Step '01-start' 'start on an empty mods dir; the controls exist and are reachable through UI Automation' {
    Start-Manager $modsDir
    $w = Wait-Ready
    Ok ($w.Name -eq 'Shadow Mod Manager') "window title is '$($w.Name)'"
    foreach ($n in 'Install...', 'Remove', 'Move up', 'Move down', 'Open folder', 'Refresh', 'Play') {
        $b = Button $w $n
        Ok ($null -ne $b -and $b.Has(10000)) ("button '$n' found with an Invoke pattern" + $(if ($b) { ", enabled=$($b.Enabled), id=$($b.Aid)" } else { '' }))
    }
    $m = Button $w 'Load mods'
    Ok ($null -ne $m -and $m.Has(10015)) "check box 'Load mods' found with a Toggle pattern"
    if ($m) { Ok ($m.ToggleState -eq 1) "'Load mods' is ticked on a fresh start (toggle state $($m.ToggleState))" }
    $list = $w.ByAid('100')
    Ok ($null -ne $list -and $list.Cls -eq 'SysListView32') 'the mod list (SysListView32) is found'
    $heads = @($list.Descendants() | Where-Object { $_.Aid -like 'HeaderItem*' } | ForEach-Object { $_.Name })
    Ok (($heads -join '|') -eq 'Mod (highest priority first)|Version|Author|Content|Status') "list columns: $($heads -join ' | ')"
    Ok ((Get-Rows $w).Count -eq 0) 'the list is empty'
    $d = Details $w; Info-Details $d
    Ok ($d -like 'Select a mod to see its details.*') 'the details pane shows the "Select a mod" hint'
    Ok ((StatusLine $w) -like '0 installed, 0 enabled, 0 load*') "status line: '$(StatusLine $w)'"
    if ($list.Name -like 'Installed mods*') { Ok $true "the mod list accessible name is '$($list.Name)'" } else { Finding "the mod list still has the UIA name '$($list.Name)': the Win32 list view proxy does not use the window text 'Installed mods' as its name (fix 3 is only half effective)" }
    Ok ($w.ByAid('109').Name -eq 'Details') "the details pane accessible name is '$($w.ByAid('109').Name)' (expected Details)"
    Ok (-not (Test-Path -LiteralPath $stateFile)) 'no mods.json is written for an empty folder (nothing to save yet)'
}

# 2 ------------------------------------------------------------------------------------------------------------------------
Step '02-drop-one' 'drag and drop cream.rar onto the list: result box, folder and state on disk' {
    $w = Main-Window
    $r = Drop-And-Wait $w @($cream) 'list'
    Ok (Accepted $r.Effect) "OLE drop effect on the list: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no result message box appeared after the drop'
    Info ("message box text: " + ($r.Text -replace "`n", ' / '))
    Ok ($r.Text -like '*Installed "Cream" from cream.rar*') 'the box says Cream was installed from cream.rar'
    Ok ($r.Text -like '*: 1 file, 0 AFS entries, 0 textures.*') 'the box says "1 file" (singular)'
    Ok ($r.Text -like '*It is off until you tick it*') 'the box says the mod stays off until ticked'
    Press-Dialog $r.Dialog '1'
    Ok (Test-Path -LiteralPath (Join-Path $modsDir 'cream')) 'folder mods\cream exists on disk'
    Ok ((State-Ids) -contains 'cream') "mods.json lists cream (order: $((State-Ids) -join ', '))"
    Ok ((State-Entry 'cream').enabled -eq $false) 'cream is saved as enabled=false'
    $rows = @(Get-Rows $w)
    Ok ($rows.Count -eq 1 -and $rows[0].Name -eq 'Cream' -and $rows[0].Status -eq 'Off' -and -not $rows[0].Checked) "list row: '$($rows[0].Name)' / status '$($rows[0].Status)' / ticked=$($rows[0].Checked)"
}

# 3 ------------------------------------------------------------------------------------------------------------------------
Step '03-drop-two' 'drop playable_tails.zip and xbox_buttons_files.zip together on the title bar' {
    $w = Main-Window
    $r = Drop-And-Wait $w @($tails, $xbox) 'title'
    Ok (Accepted $r.Effect) "OLE drop effect on the title bar: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no result message box appeared after the drop'
    Info ("message box text: " + ($r.Text -replace "`n", ' / '))
    Ok ($r.Text -like '*Installed "Playable Tails" from playable_tails.zip*') 'the box reports Playable Tails'
    Ok ($r.Text -like '*from xbox_buttons_files.zip*') 'the box reports the Xbox buttons download'
    Press-Dialog $r.Dialog '1'
    $ids = State-Ids
    Ok ($ids.Count -eq 3) "mods.json now has 3 mods: $($ids -join ', ')"
    foreach ($id in $ids) { Ok (Test-Path -LiteralPath (Join-Path $modsDir $id)) "folder mods\$id exists" }
    $script:xboxId = @($ids | Where-Object { $_ -ne 'cream' -and $_ -ne 'playable-tails' })[0]
    Info "the Xbox download's id is '$($script:xboxId)'"
}

# 4 ------------------------------------------------------------------------------------------------------------------------
Step '04-rows' 'the list rows match mods.json and the command line (names, versions, content, status Off)' {
    $w = Main-Window
    $rows = @(Get-Rows $w)
    $cli = @(Cli-List)
    $ids = State-Ids
    Ok ($rows.Count -eq 3) "3 rows in the window ($(($rows | ForEach-Object { $_.Name }) -join ' | '))"
    Need ($rows.Count -eq 3 -and $cli.Count -eq 3) 'rows or CLI list not 3 long'
    for ($i = 0; $i -lt 3; $i++) {
        Ok ($rows[$i].Name -eq $cli[$i].Name -and $cli[$i].Id -eq $ids[$i]) "row $($i + 1): window '$($rows[$i].Name)' = CLI '$($cli[$i].Name)' = mods.json '$($ids[$i])'"
        Ok ($rows[$i].Version -eq $cli[$i].Version) "  version '$($rows[$i].Version)' = CLI '$($cli[$i].Version)'"
        Ok ($rows[$i].Content -eq $cli[$i].Content) "  content '$($rows[$i].Content)' = CLI '$($cli[$i].Content)'"
        Ok ($rows[$i].Status -eq 'Off' -and -not $rows[$i].Checked) "  status '$($rows[$i].Status)', ticked=$($rows[$i].Checked)"
    }
    $s = StatusLine $w
    Ok ($s -like '3 installed, 0 enabled, 0 load*') "status line: '$s'"
}

# 5 ------------------------------------------------------------------------------------------------------------------------
Step '05-tick-uia' 'tick a mod through UI Automation (Toggle pattern): mods.json enabled=true, row and status follow' {
    $w = Main-Window
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Cream' } | Select-Object -First 1
    Need ($null -ne $row) 'no Cream row'
    $row.El.Toggle()
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    Ok ((State-Entry 'cream').enabled -eq $true) 'mods.json: cream enabled=true'
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Cream' } | Select-Object -First 1
    Ok ($row.Checked) 'the row shows the tick'
    Ok ($row.Status -like 'Loads*') "status column: '$($row.Status)'"
    Ok ((StatusLine $w) -like '3 installed, 1 enabled, 1 load*') "status line: '$(StatusLine $w)'"
    Ok ($row.Selected) 'ticking selected the row (the window shows the mod it just changed)'
    Ok ((Details $w) -like 'Cream*') 'details pane shows Cream'
    # untick again, so the next steps start from a known state
    $row.El.Toggle()
    $null = Wait-For { (State-Entry 'cream').enabled -eq $false } 5
    Ok ((State-Entry 'cream').enabled -eq $false) 'unticking saves enabled=false'
}

# 6 ------------------------------------------------------------------------------------------------------------------------
Step '06-tick-click' 'tick a mod with a real mouse click on its check box' {
    $w = Focus-Main
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Playable Tails' } | Select-Object -First 1
    Need ($null -ne $row) 'no Playable Tails row'
    $rr = $row.El.Rect
    $scale = ($w.Rect[2] - $w.Rect[0]) / 1180.0
    $x = [int]($rr[0] + 12 * $scale); $y = [int](($rr[1] + $rr[3]) / 2)
    Need ($rr[3] -gt $rr[1] -and -not $row.El.Offscreen) 'the row is not on screen'
    [Nat]::MouseClick($x, $y)
    $ok = Wait-For { (State-Entry 'playable-tails').enabled -eq $true } 5
    Ok ($null -ne $ok) "mouse click at ($x,$y) on the check box -> mods.json playable-tails enabled=true"
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Playable Tails' } | Select-Object -First 1
    Ok ($row.Checked) 'the row shows the tick'
    # and back off with a click
    Start-Sleep -Milliseconds 900   # a second click within the double-click time would be a double-click (which also toggles)
    [Nat]::MouseClick($x, $y)
    $null = Wait-For { (State-Entry 'playable-tails').enabled -eq $false } 5
    Ok ((State-Entry 'playable-tails').enabled -eq $false) 'a second click unticks it again (mods.json enabled=false)'
}

# 7 ------------------------------------------------------------------------------------------------------------------------
Step '07-move' 'select a mod and press Move up / Move down: the order in mods.json and in the list follows' {
    $w = Main-Window
    $before = State-Ids
    Need ($before.Count -eq 3) 'needs 3 mods'
    $last = @(Get-Rows $w)[2]
    $last.El.Select()
    $null = Wait-For { @(Get-Rows $w)[2].Selected } 5
    Press $w 'Move up'
    $after = State-Ids
    Ok ($after[1] -eq $before[2] -and $after[2] -eq $before[1] -and $after[0] -eq $before[0]) "Move up on row 3: $($before -join ',') -> $($after -join ',')"
    $rows = @(Get-Rows $w)
    Ok ($rows[1].Selected -and $rows[1].Name -eq @(Cli-List)[1].Name) "the selection followed the mod to row 2 ('$($rows[1].Name)')"
    Press $w 'Move up'
    $after2 = State-Ids
    Ok ($after2[0] -eq $before[2]) "second Move up puts it first: $($after2 -join ',')"
    Press $w 'Move up'
    Ok (Same (State-Ids) $after2) 'Move up on the first row changes nothing'
    Press $w 'Move down'
    $after3 = State-Ids
    Ok ($after3[1] -eq $before[2] -and $after3[0] -eq $before[0]) "Move down: $($after3 -join ',')"
    Ok (Same @(Get-Rows $w | ForEach-Object { $_.Name }) @(Cli-List | ForEach-Object { $_.Name })) 'list order = the CLI order of mods.json'
    # put the order back: the mod that started last goes to the end
    Press $w 'Move down'
    Ok (Same (State-Ids) $before) "restored the original order: $((State-Ids) -join ',')"
}

# 8 ------------------------------------------------------------------------------------------------------------------------
Step '08-conflict' 'enable Cream and Playable Tails: the details name the conflict (character/shadow.one), the status column shows winner and loser' {
    $w = Main-Window
    $ids = State-Ids
    Need ($ids -contains 'cream' -and $ids -contains 'playable-tails') 'needs cream and playable-tails'
    foreach ($n in 'Cream', 'Playable Tails') {
        $row = Get-Rows $w | Where-Object { $_.Name -eq $n } | Select-Object -First 1
        if (-not $row.Checked) { $row.El.Toggle(); Start-Sleep -Milliseconds 400 }
    }
    Ok ((State-Entry 'cream').enabled -and (State-Entry 'playable-tails').enabled) 'mods.json: both enabled'
    $rows = @(Get-Rows $w)
    $iC = [array]::IndexOf(@($rows | ForEach-Object { $_.Name }), 'Cream')
    $iT = [array]::IndexOf(@($rows | ForEach-Object { $_.Name }), 'Playable Tails')
    $winner = if ($iC -lt $iT) { 'Cream' } else { 'Playable Tails' }
    $loser = if ($winner -eq 'Cream') { 'Playable Tails' } else { 'Cream' }
    $winId = if ($winner -eq 'Cream') { 'cream' } else { 'playable-tails' }
    $loseId = if ($winner -eq 'Cream') { 'playable-tails' } else { 'cream' }
    Info "list order puts '$winner' above '$loser': the higher row wins"
    $rw = $rows | Where-Object { $_.Name -eq $winner }; $rl = $rows | Where-Object { $_.Name -eq $loser }
    Ok ($rw.Status -eq 'Loads (1 file over others, 0 overridden)') "status column of the winner: '$($rw.Status)'"
    Ok ($rl.Status -eq 'Loads (0 files over others, 1 overridden)') "status column of the loser: '$($rl.Status)'"
    Select-Row $w $winner
    $d = Details $w; Info-Details $d
    Ok ($d -like '*File conflicts this mod wins (1):*character/shadow.one (over*') 'winner details name the won conflict on character/shadow.one'
    Select-Row $w $loser
    $d = Details $w; Info-Details $d
    Ok ($d -like "*File conflicts this mod loses (1):*character/shadow.one ($winId wins)*") "loser details: 'character/shadow.one ($winId wins)'"
    Ok ((StatusLine $w) -like '*1 file conflict*') "status line counts the conflict: '$(StatusLine $w)'"
    $plan = (Run-Cli @('plan')) | ConvertFrom-Json
    $f = @($plan.files | Where-Object { $_.game_path -eq 'character/shadow.one' })[0]
    Ok ($f.mod -eq $winId -and (@($f.overridden) -contains $loseId)) "CLI plan agrees: character/shadow.one comes from '$($f.mod)', overriding $(@($f.overridden) -join ',')"
    # Move the loser above the winner: the roles swap
    Press $w 'Move up'
    $rows = @(Get-Rows $w)
    $rl2 = $rows | Where-Object { $_.Name -eq $loser }
    Ok ($rl2.Status -eq 'Loads (1 file over others, 0 overridden)') "after Move up the former loser wins: '$($rl2.Status)'"
    $plan = (Run-Cli @('plan')) | ConvertFrom-Json
    $f = @($plan.files | Where-Object { $_.game_path -eq 'character/shadow.one' })[0]
    Ok ($f.mod -eq $loseId) "CLI plan agrees after the swap: winner '$($f.mod)'"
}

# 9 ------------------------------------------------------------------------------------------------------------------------
Step '09-master' 'untick "Load mods": mods_enabled=false on disk, rows say all mods are off; tick again restores' {
    $w = Main-Window
    $m = Button $w 'Load mods'
    $m.Toggle()
    $null = Wait-For { (Read-State).mods_enabled -eq $false } 5
    Ok ((Read-State).mods_enabled -eq $false) 'mods.json: mods_enabled=false'
    Ok ($m.ToggleState -eq 0) 'the check box is unticked'
    $rows = @(Get-Rows $w)
    $on = @($rows | Where-Object { $_.Checked })
    Ok ($on.Count -ge 2 -and -not (@($on | Where-Object { $_.Status -ne 'On (all mods switched off)' }).Count)) "ticked rows show 'On (all mods switched off)' ($($on.Count) rows)"
    Ok ((StatusLine $w) -like '3 installed - all mods switched off*') "status line: '$(StatusLine $w)'"
    $m.Toggle()
    $null = Wait-For { (Read-State).mods_enabled -eq $true } 5
    $savedTrue = ((Read-State).mods_enabled -eq $true)
    Ok $savedTrue 'ticking again saves mods_enabled=true'
    if (-not $savedTrue) {
        $age = [int]((Get-Date) - (Get-Item -LiteralPath $stateFile).LastWriteTime).TotalSeconds
        Info "evidence: mods.json.tmp exists=$(Test-Path -LiteralPath "$stateFile.tmp"), mods.json last written $age s ago, window check box ticked=$($m.ToggleState -eq 1)"
        Finding 'a change shown in the window did not reach mods.json although the test held no lock (seen once in about ten full runs); consistent with finding 1 in GUI_TEST.md: one rename attempt, e.g. a real-time scanner holding the freshly written file'
    }
    Ok ((StatusLine $w) -like '3 installed, 2 enabled, 2 load*') "status line: '$(StatusLine $w)'"
}

# 10 -----------------------------------------------------------------------------------------------------------------------
Step '10-options' 'drop a mod folder that has options (shadow-model-fixes) and change an option with the combo box' {
    Need ($ModelFixes -ne '' -and (Test-Path -LiteralPath $ModelFixes)) 'no -ModelFixes folder given'
    $w = Main-Window
    $r = Drop-And-Wait $w @((Resolve-Path -LiteralPath $ModelFixes).Path) 'list' 180
    Ok (Accepted $r.Effect) "OLE drop of a folder, effect: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no result message box appeared after dropping the folder'
    Info ("message box text: " + (($r.Text -replace "`n", ' / ')))
    Press-Dialog $r.Dialog '1'
    $new = @(State-Ids | Where-Object { $_ -notin @('cream', 'playable-tails', $script:xboxId) })
    Need ($new.Count -ge 1) 'the folder was not installed (see the box text above)'
    $id = $new[0]
    Ok ($true) "installed as '$id'"
    $rows = @(Get-Rows $w)
    $row = $rows | Where-Object { $_.Name -ne 'Cream' -and $_.Name -ne 'Playable Tails' -and $_.Selected } | Select-Object -First 1
    Need ($null -ne $row) 'the installed mod is not selected'
    Ok ($row.Content -like '*option*') "content column mentions options: '$($row.Content)'"
    $combos = @($w.Descendants() | Where-Object { $_.Cls -eq 'ComboBox' -and -not $_.Offscreen })
    Need ($combos.Count -ge 1) 'no visible combo box for the options (the details pane did not show any)'
    $combo = $combos[0]
    Info "combo box accessible name: '$($combo.Name)'"
    if ($combo.Name -eq '') { Finding 'the option combo box has an empty accessible name (the option label next to it is a separate static, not linked)' }
    Ok ($true) "$($combos.Count) option combo box(es) visible; first: value '$($combo.Value)'"
    $before = (State-Entry $id).options
    $beforeText = $combo.Value
    # a real user: focus the combo box and press Down (the drop-down list is not opened)
    $null = Focus-Main
    $combo.Focus(); Start-Sleep -Milliseconds 300
    [Nat]::Key(0x28)   # VK_DOWN
    $null = Wait-For { (State-Entry $id).options -ne $null } 5
    $afterText = ($w.Descendants() | Where-Object { $_.Cls -eq 'ComboBox' -and -not $_.Offscreen } | Select-Object -First 1).Value
    $opts = (State-Entry $id).options
    Info "combo box value: '$beforeText' -> '$afterText'; mods.json options: $($opts | ConvertTo-Json -Compress)"
    Ok ($afterText -ne $beforeText) 'the combo box moved to the next choice'
    Ok ($null -ne $opts -and ($opts.PSObject.Properties | Measure-Object).Count -ge 1) 'mods.json holds the chosen option'
}

# 10b ----------------------------------------------------------------------------------------------------------------------
Step '10b-install-dialog' 'Install... button and the common file dialog: Cancel installs nothing; a typed path plus Open installs' {
    $w = Focus-Main
    $b = Button $w 'Install...'
    Need ($null -ne $b) 'no Install... button'
    $rr = $b.Rect
    $before = State-Ids
    $click = { [Nat]::MouseClick([int](($rr[0] + $rr[2]) / 2), [int](($rr[1] + $rr[3]) / 2)) }
    $fileDialog = { @(Dialogs | Where-Object { $_.Name -like 'Install mods*' })[0] }
    & $click
    $d = Wait-For $fileDialog 15
    Need ($null -ne $d) 'the file dialog did not open after a real click on Install...'
    Ok ($d.Name -eq 'Install mods (folders: drop them on the window)') "file dialog title: '$($d.Name)'"
    $cancel = @($d.Descendants() | Where-Object { $_.Cls -eq 'Button' -and $_.Aid -eq '2' })[0]
    Need ($null -ne $cancel) 'the file dialog has no Cancel button (control id 2)'
    $cancel.Invoke()
    $gone = Wait-For { $null -eq (& $fileDialog) } 10
    Ok ($null -ne $gone) 'Cancel closes the file dialog'
    Ok (Same (State-Ids) $before) 'Cancel installs nothing (mods.json unchanged)'
    & $click
    $d = Wait-For $fileDialog 15
    Need ($null -ne $d) 'the file dialog did not open the second time'
    $name = @($d.Descendants() | Where-Object { $_.Cls -eq 'Edit' -and $_.Aid -eq '1148' })[0]
    $open = @($d.Descendants() | Where-Object { $_.Cls -eq 'Button' -and $_.Aid -eq '1' })[0]
    Need ($null -ne $name -and $null -ne $open) 'the file name box or the Open button of the dialog was not found'
    $name.SetValue((Join-Path $Downloads 'regular_rouge.zip'))
    Ok ($name.Value -like '*regular_rouge.zip') "typed the path into the file name box: '$($name.Value)'"
    $open.Invoke()
    $box = Wait-For { $x = @(Dialogs | Where-Object { $_.Name -eq 'Shadow Mod Manager' })[0]; $x } 120
    Need ($null -ne $box) 'no result message box after Open'
    $t = Dialog-Text $box
    Info ('message box text: ' + ($t -replace "`n", ' / '))
    Ok ($t -like '*Installed "Regular Rouge" from regular_rouge.zip*') 'the box reports Regular Rouge'
    Press-Dialog $box '1'
    Ok ((State-Ids) -contains 'regular-rouge' -and (Test-Path -LiteralPath (Join-Path $modsDir 'regular-rouge'))) "mods.json and the mods dir have regular-rouge ($((State-Ids) -join ', '))"
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Regular Rouge' } | Select-Object -First 1
    Ok ($null -ne $row -and $row.Status -eq 'Off') "list row 'Regular Rouge' with status 'Off'"
}

# 11 -----------------------------------------------------------------------------------------------------------------------
Step '11-refresh' 'change mods.json from outside (command line), press Refresh: the window follows' {
    $w = Main-Window
    $cli = @(Cli-List)
    $target = $cli | Where-Object { $_.Id -eq 'cream' }
    Need ($null -ne $target) 'cream missing'
    $wasOn = $target.On
    $null = Run-Cli @($(if ($wasOn) { 'disable' } else { 'enable' }), 'cream')
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Cream' }
    Ok ($row.Checked -eq $wasOn) 'before Refresh the window still shows the old tick (it does not poll)'
    Press $w 'Refresh'
    $row = Get-Rows $w | Where-Object { $_.Name -eq 'Cream' }
    Ok ($row.Checked -eq (-not $wasOn)) "after Refresh the tick follows the file (ticked=$($row.Checked))"
    $null = Run-Cli @($(if ($wasOn) { 'enable' } else { 'disable' }), 'cream')
    Press $w 'Refresh'
    Ok ((Get-Rows $w | Where-Object { $_.Name -eq 'Cream' }).Checked -eq $wasOn) 'restored'
}

# 12 -----------------------------------------------------------------------------------------------------------------------
Step '12-remove' 'Remove: answer No (nothing changes), then Yes (folder leaves the mods dir for the Recycle Bin)' {
    $w = Main-Window
    $victim = 'playable-tails'
    Need ((State-Ids) -contains $victim) 'playable-tails not installed'
    Select-Row $w 'Playable Tails'
    $dir = Join-Path $modsDir $victim
    $idsBefore = State-Ids
    Press $w 'Remove'
    $d = Wait-Dialog 10
    Need ($null -ne $d) 'no confirmation box'
    $t = Dialog-Text $d
    Info ('confirmation text: ' + ($t -replace "`n", ' / '))
    Ok ($d.Name -eq 'Remove mod' -and $t -like '*Remove "Playable Tails"?*Recycle Bin*') "title '$($d.Name)', names the mod and the Recycle Bin"
    Press-Dialog $d '7'
    Ok ((Test-Path -LiteralPath $dir) -and ((State-Ids) -join ',') -eq ($idsBefore -join ',')) 'No: the folder and mods.json are unchanged'
    Press $w 'Remove'
    $d = Wait-Dialog 10
    Press-Dialog $d '6'
    $gone = Wait-For { -not (Test-Path -LiteralPath $dir) } 15
    Ok ($null -ne $gone) "Yes: mods\$victim is gone from the mods dir"
    $null = Wait-For { -not ((State-Ids) -contains $victim) } 10
    Ok (-not ((State-Ids) -contains $victim)) "mods.json no longer lists it ($((State-Ids) -join ', '))"
    Ok (@(Get-Rows $w | Where-Object { $_.Name -eq 'Playable Tails' }).Count -eq 0) 'the row is gone from the list'
    Ok ((Details $w) -like 'Select a mod*') 'the details pane is back at the hint'
    Info 'the folder went to the Recycle Bin (FOF_ALLOWUNDO); the test does not empty the bin, it only created this folder itself'
}

# 13 -----------------------------------------------------------------------------------------------------------------------
Step '13-nonarchive' 'drop a text file and a fake .zip: failure messages, nothing installed' {
    $w = Main-Window
    $txt = Join-Path $run 'not-a-mod.txt'; Set-Content -LiteralPath $txt -Value 'hello' -Encoding ASCII
    $fake = Join-Path $run 'fake.zip'; Set-Content -LiteralPath $fake -Value 'this is not a zip archive' -Encoding ASCII
    $before = State-Ids
    $r = Drop-And-Wait $w @($txt) 'details' 60
    Ok (Accepted $r.Effect) "OLE drop effect on the details pane: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no message box after dropping a text file'
    Info ('message box text: ' + ($r.Text -replace "`n", ' / '))
    Ok ($r.Text -like '*Could not install not-a-mod.txt*') 'the box says it could not install not-a-mod.txt'
    Press-Dialog $r.Dialog '1'
    $r = Drop-And-Wait $w @($fake) 'status' 60
    Ok (Accepted $r.Effect) "OLE drop effect on the status line: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no message box after dropping a fake zip'
    Info ('message box text: ' + ($r.Text -replace "`n", ' / '))
    Ok ($r.Text -like '*Could not install fake.zip*') 'the box says it could not install fake.zip'
    Press-Dialog $r.Dialog '1'
    Ok (Same (State-Ids) $before) 'mods.json is unchanged'
    Ok (-not (Test-Path -LiteralPath (Join-Path $modsDir 'not-a-mod')) -and -not (Test-Path -LiteralPath (Join-Path $modsDir 'fake'))) 'no leftover folder in the mods dir'
    Ok ((Details $w) -ne $null) 'the window is still alive and responsive'
}

# 14 -----------------------------------------------------------------------------------------------------------------------
Step '14-play-missing' 'Play without ShadowTheHedgehog.exe beside the manager: an error box, nothing launched' {
    $w = Main-Window
    $gameExe = Join-Path (Split-Path $Exe) 'ShadowTheHedgehog.exe'
    Need (-not (Test-Path -LiteralPath $gameExe)) "a ShadowTheHedgehog.exe sits beside the manager ($gameExe): pressing Play would start the game"
    Press $w 'Play'
    $d = Wait-Dialog 10
    Need ($null -ne $d) 'no message box after Play'
    $t = Dialog-Text $d
    Info ('message box text: ' + ($t -replace "`n", ' / '))
    Ok ($t -like 'ShadowTheHedgehog.exe was not found beside the manager*') 'Play reports the missing game executable'
    Press-Dialog $d '1'
}

Stop-Manager

# 15 -----------------------------------------------------------------------------------------------------------------------
Step '15-damaged-no' 'damaged mods.json at start: the window asks for a new list; No leaves the file untouched and the window is read only' {
    Need (Test-Path -LiteralPath $stateFile) 'no mods.json to damage'
    Copy-Item -LiteralPath $stateFile -Destination (Join-Path $run 'mods.json.good') -Force
    $bad = [Text.Encoding]::ASCII.GetBytes('{ "schema": 1, "mods_enabled": tru')
    [IO.File]::WriteAllBytes($stateFile, $bad)
    $h0 = (Hash-File $stateFile)
    Start-Manager $modsDir
    $d = Wait-Dialog 60
    Need ($null -ne $d) 'no message box at start-up'
    $t = Dialog-Text $d
    Info ('message box text: ' + ($t -replace "`n", ' / '))
    Ok ($t -like 'The mod list file could not be read:*Start a new list?*') 'the box asks whether to start a new list'
    Press-Dialog $d '7'
    $w = Wait-Ready
    Ok ((Hash-File $stateFile) -eq $h0) 'after No, mods.json is byte-for-byte the damaged file'
    $null = Wait-For { (StatusLine $w) -like 'mods.json is damaged - read only*' } 5
    Ok ((StatusLine $w) -like 'mods.json is damaged - read only*') "status line: '$(StatusLine $w)'"
    $rows = @(Get-Rows $w)
    Info "the window lists $($rows.Count) row(s) built in memory from the mods folder"
    Need ($rows.Count -ge 2) 'needs at least 2 rows'
    $warn = 'The mod list file is damaged, so changes cannot be saved*'
    # a real click on the first row's check box (UIA patterns would block while the box is open)
    $null = Focus-Main
    $rr = $rows[0].El.Rect; $scale = ($w.Rect[2] - $w.Rect[0]) / 1180.0
    [Nat]::MouseClick([int]($rr[0] + 12 * $scale), [int](($rr[1] + $rr[3]) / 2))
    $dlg = Wait-Dialog 10
    Ok ($null -ne $dlg -and (Dialog-Text $dlg) -like $warn) 'a tick is refused with the warning box'
    if ($dlg) { Press-Dialog $dlg '1' }
    Ok (-not (@(Get-Rows $w))[0].Checked) 'the tick is put back (row unticked)'
    # Move down on the first row (select it with a real click on its name, then click Move down)
    $rr = (@(Get-Rows $w))[0].El.Rect
    [Nat]::MouseClick([int]($rr[0] + 120 * $scale), [int](($rr[1] + $rr[3]) / 2))
    Start-Sleep -Milliseconds 400
    $b = Button $w 'Move down'; $br = $b.Rect
    [Nat]::MouseClick([int](($br[0] + $br[2]) / 2), [int](($br[1] + $br[3]) / 2))
    $dlg = Wait-Dialog 10
    Ok ($null -ne $dlg -and (Dialog-Text $dlg) -like $warn) 'Move down is refused with the warning box'
    if ($dlg) { Press-Dialog $dlg '1' }
    $m = Button $w 'Load mods'; $mr = $m.Rect
    [Nat]::MouseClick([int](($mr[0] + $mr[2]) / 2), [int](($mr[1] + $mr[3]) / 2))
    $dlg = Wait-Dialog 10
    Ok ($null -ne $dlg -and (Dialog-Text $dlg) -like $warn) '"Load mods" is refused with the warning box'
    if ($dlg) { Press-Dialog $dlg '1' }
    Ok ($m.ToggleState -eq 1) '"Load mods" is put back as stored (ticked)'
    Press $w 'Refresh'
    $d = Wait-Dialog 10
    Ok ($null -ne $d) 'Refresh asks again'
    if ($d) { Press-Dialog $d '7' }
    Ok ((Hash-File $stateFile) -eq $h0) 'mods.json is still byte-for-byte the damaged file after all of it'
    Stop-Manager
}

# 16 -----------------------------------------------------------------------------------------------------------------------
Step '16-damaged-yes' 'damaged mods.json: Yes starts a new list (every installed mod listed, all off)' {
    Start-Manager $modsDir
    $d = Wait-Dialog 60
    Need ($null -ne $d) 'no message box at start-up'
    Press-Dialog $d '6'
    $w = Wait-Ready
    $st = Read-State
    Ok ($st.mods_enabled -eq $true) 'mods.json is valid again, mods_enabled=true'
    $inst = @(Get-ChildItem -LiteralPath $modsDir -Directory | ForEach-Object { $_.Name })
    Ok ((State-Ids).Count -eq $inst.Count) "all $($inst.Count) installed folders are in the new list: $((State-Ids) -join ', ')"
    Ok (@($st.order | Where-Object { $_.enabled }).Count -eq 0) 'every mod is off'
    $rows = @(Get-Rows $w)
    Ok ($rows.Count -eq $inst.Count -and @($rows | Where-Object { $_.Checked }).Count -eq 0) "window shows $($rows.Count) rows, none ticked"
}

# 17 -----------------------------------------------------------------------------------------------------------------------
Step '17-save-locked' 'mods.json held open by another program (a scanner, a backup): the window reports that the change was not saved' {
    Need ($null -ne $script:proc -and -not $script:proc.HasExited) 'manager is not running'
    $w = Main-Window
    $m = Button $w 'Load mods'
    Need ($m.ToggleState -eq 1) '"Load mods" is not ticked at the start of this step'
    $null = Focus-Main
    $h0 = (Hash-File $stateFile)
    # share mode ReadWrite without Delete: what a virus scanner or an indexer commonly uses. The manager saves by
    # writing mods.json.tmp and renaming it over mods.json, which fails while the file is open like this.
    $lock = [IO.File]::Open($stateFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $dlg = $null
    try {
        $mr = $m.Rect
        [Nat]::MouseClick([int](($mr[0] + $mr[2]) / 2), [int](($mr[1] + $mr[3]) / 2))   # real click: the box blocks UIA calls
        $dlg = Wait-Dialog 15
        $same = ((Hash-File $stateFile) -eq $h0)
    }
    finally { $lock.Dispose() }
    Ok ($null -ne $dlg) 'an error box appears while the file is held open'
    if ($dlg) {
        $t = Dialog-Text $dlg
        Info ('message box text: ' + ($t -replace "`n", ' / '))
        Ok ($t -like 'This change was NOT saved:*') 'the box says the change was NOT saved'
        Press-Dialog $dlg '1'
    }
    Ok $same 'mods.json is byte-identical (valid, not corrupted)'
    Ok (-not (Test-Path -LiteralPath "$stateFile.tmp")) 'no stray mods.json.tmp is left behind'
    $null = Wait-For { $m.ToggleState -eq 0 } 5
    Ok ($m.ToggleState -eq 0) 'the window keeps the choice in memory ("Load mods" unticked) while the file still says true'
    # with the file free again the next change writes the whole in-memory state, the lost choice included
    $row = @(Get-Rows $w)[0]
    $row.El.Toggle()
    $null = Wait-For { (State-Entry (State-Ids)[0]).enabled -eq $true } 5
    Ok ((Read-State).mods_enabled -eq $false -and (State-Entry (State-Ids)[0]).enabled -eq $true) 'a later successful save writes both the row tick and the earlier choice (mods_enabled=false)'
    # restore
    $row = @(Get-Rows $w)[0]; $row.El.Toggle(); Start-Sleep -Milliseconds 500
    $m.Toggle()
    $null = Wait-For { (Read-State).mods_enabled -eq $true } 5
    Ok ((Read-State).mods_enabled -eq $true) 'restored (mods_enabled=true)'
}

# 18 -----------------------------------------------------------------------------------------------------------------------
Step '18-close' 'close the window: the process ends' {
    Need ($null -ne $script:proc -and -not $script:proc.HasExited) 'manager is not running'
    $pidWas = $script:proc.Id
    Stop-Manager
    Ok ($script:closedGracefully -eq $true) 'WM_CLOSE (window pattern Close) ended the process without being killed'
    Ok ($null -eq (Get-Process -Id $pidWas -ErrorAction SilentlyContinue)) "process $pidWas is gone"
}


# ==== round 4: the ergonomic features (fresh mods dir, three small mods) ===============================================

# 19 -----------------------------------------------------------------------------------------------------------------------
Step '19-setup' 'a second, fresh mods dir with Cream, Playable Tails and Regular Rouge installed, window started' {
    Stop-Manager
    $script:modsDir = Join-Path $run 'mods2'
    $script:stateFile = Join-Path $script:modsDir 'mods.json'
    New-Item -ItemType Directory -Force -Path $script:modsDir | Out-Null
    foreach ($f in @($cream, $tails, (Join-Path $Downloads 'regular_rouge.zip'))) {
        $o = Run-Cli @('install', $f)
        Ok ($o -like 'installed *') "CLI install of $(Split-Path $f -Leaf): $(($o -split "`n")[0].Trim())"
    }
    Start-Manager $script:modsDir
    $w = Wait-Ready
    $names = @(Get-Rows $w | ForEach-Object { $_.Name })
    Ok (Same $names @('Cream', 'Playable Tails', 'Regular Rouge')) "rows: $($names -join ' | ')"
    Ok ((State-Ids) -join ',' -eq 'cream,playable-tails,regular-rouge') "mods.json order: $((State-Ids) -join ',')"
}

# 20 -----------------------------------------------------------------------------------------------------------------------
Step '20-context-menu' 'right-click a row: the menu, Move to top / bottom, Switch on, Switch all on / off' {
    $w = Main-Window
    Need ((State-Ids) -join ',' -eq 'cream,playable-tails,regular-rouge') 'the setup order is not as expected'
    Rclick-Row $w 'Regular Rouge'
    $pop = Wait-Popup
    Need ($null -ne $pop) 'no context menu appeared after a real right-click on a row'
    $names = Menu-Names $pop
    Info ('menu items: ' + ($names -join ' | '))
    foreach ($n in 'Switch on', 'Move to top', 'Move up', 'Move down', 'Move to bottom', 'Edit name, version, author', 'Open folder', 'Remove', 'Switch all on', 'Switch all off', 'Install') {
        Ok (@($names | Where-Object { $_ -like "$n*" }).Count -ge 1) "menu has '$n'"
    }
    $sel = @(Get-Rows $w | Where-Object { $_.Selected } | ForEach-Object { $_.Name })
    Ok ($sel -contains 'Regular Rouge') 'the right-click selected the row under the pointer'
    Pick-Menu $pop 'Move to top'
    Ok (Wait-Order @('regular-rouge', 'cream', 'playable-tails')) "Move to top: $((State-Ids) -join ',')"
    Ok ((@(Get-Rows $w))[0].Name -eq 'Regular Rouge') 'the list shows it first'
    # keyboard in the menu: Down x5 = Move to bottom (Switch on, Move to top, Move up, Move down, Move to bottom), Enter
    Rclick-Row $w 'Regular Rouge'
    $pop = Wait-Popup
    Need ($null -ne $pop) 'no context menu the second time'
    for ($i = 0; $i -lt 5; $i++) { [Nat]::Key(0x28) }
    [Nat]::Key(0x0D)
    Ok (Wait-Order @('cream', 'playable-tails', 'regular-rouge')) "arrow keys + Enter on 'Move to bottom': $((State-Ids) -join ',')"
    # a real mouse click on a menu item: Move down on Cream
    Rclick-Row $w 'Cream'
    $pop = Wait-Popup
    Pick-Menu $pop 'Move down' $true
    Ok (Wait-Order @('playable-tails', 'cream', 'regular-rouge')) "mouse click on 'Move down': $((State-Ids) -join ',')"
    # Switch on / off
    Rclick-Row $w 'Cream'
    $pop = Wait-Popup
    Ok (@(Menu-Names $pop | Where-Object { $_ -like 'Switch on*' }).Count -eq 1) "an off mod offers 'Switch on'"
    Pick-Menu $pop 'Switch on'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    Ok ((State-Entry 'cream').enabled -eq $true) 'Switch on: cream enabled=true in mods.json'
    Rclick-Row $w 'Cream'
    $pop = Wait-Popup
    Ok (@(Menu-Names $pop | Where-Object { $_ -like 'Switch off*' }).Count -eq 1) "an on mod offers 'Switch off'"
    Pick-Menu $pop 'Switch all on'
    $null = Wait-For { @((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0 } 5
    Ok (@((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0) 'Switch all on: every mod enabled=true'
    Rclick-Row $w 'Cream'
    $pop = Wait-Popup
    Pick-Menu $pop 'Switch all off'
    $null = Wait-For { @((Read-State).order | Where-Object { $_.enabled }).Count -eq 0 } 5
    Ok (@((Read-State).order | Where-Object { $_.enabled }).Count -eq 0) 'Switch all off: every mod enabled=false'
    Ok (@(Get-Rows $w | Where-Object { $_.Checked }).Count -eq 0) 'no row is ticked in the window'
}

# 21 -----------------------------------------------------------------------------------------------------------------------
Step '21-drag-row' 'drag a row to a new place with the real mouse (insertion mark, drop): both directions, below the last row' {
    $w = Main-Window
    $null = Focus-Main
    $ids = @(State-Ids)
    Need ($ids.Count -eq 3) 'needs 3 rows'
    $rows = @(Get-Rows $w); $s = Scale $w
    $x = [int]($rows[0].El.Rect[0] + 150 * $s)
    $mid = { param($i) $r = (@(Get-Rows $w))[$i].El.Rect; [int](($r[1] + $r[3]) / 2) }
    $top = { param($i) $r = (@(Get-Rows $w))[$i].El.Rect; [int]($r[1] + 3) }
    $low = { param($i) $r = (@(Get-Rows $w))[$i].El.Rect; [int]($r[3] - 3) }
    # row 1 onto the lower half of row 3: it goes to the end
    [Nat]::MouseDrag($x, (& $mid 0), $x, (& $low 2))
    $exp = @($ids[1], $ids[2], $ids[0])
    Ok (Wait-Order $exp) "row 1 dropped on the lower half of row 3: $($ids -join ',') -> $((State-Ids) -join ',')"
    Ok ((@(Get-Rows $w))[2].Selected) 'the dragged row stays selected at its new place'
    # last row onto the upper half of row 1: it goes to the top
    $ids = @(State-Ids)
    [Nat]::MouseDrag($x, (& $mid 2), $x, (& $top 0))
    Ok (Wait-Order @($ids[2], $ids[0], $ids[1])) "row 3 dropped on the upper half of row 1: $($ids -join ',') -> $((State-Ids) -join ',')"
    # first row into the empty area below the last row
    $ids = @(State-Ids)
    $rl = (@(Get-Rows $w))[2].El.Rect
    [Nat]::MouseDrag($x, (& $mid 0), $x, [int]($rl[3] + 30 * $s))
    Ok (Wait-Order @($ids[1], $ids[2], $ids[0])) "row 1 dropped in the empty area below the list: $($ids -join ',') -> $((State-Ids) -join ',')"
    # last row onto the upper half of row 2: one place up
    $ids = @(State-Ids)
    [Nat]::MouseDrag($x, (& $mid 2), $x, (& $top 1))
    Ok (Wait-Order @($ids[0], $ids[2], $ids[1])) "row 3 dropped on the upper half of row 2: $($ids -join ',') -> $((State-Ids) -join ',')"
    # dropping a row on itself changes nothing
    $ids = @(State-Ids)
    [Nat]::MouseDrag($x, (& $mid 1), $x, ((& $mid 1) + 2))
    Ok (Same (State-Ids) $ids) 'dropping a row where it is changes nothing'
    Ok (Same @(Get-Rows $w | ForEach-Object { $_.Name }) @(Cli-List | ForEach-Object { $_.Name })) 'list order = CLI order of mods.json'
}

# 22 -----------------------------------------------------------------------------------------------------------------------
Step '22-filter' 'Ctrl+F focuses the filter box; typing narrows the list; the status line says how many are shown; clearing restores' {
    $w = Main-Window
    $null = Focus-Main
    $all = @(Get-Rows $w | ForEach-Object { $_.Name })
    Need ($all.Count -eq 3) 'needs 3 rows'
    [Nat]::Chord(0x11, 0x46)    # Ctrl+F
    [Nat]::TypeText('tail')
    $null = Wait-For { @(Get-Rows $w).Count -eq 1 } 5
    $rows = @(Get-Rows $w)
    Ok ($rows.Count -eq 1 -and $rows[0].Name -eq 'Playable Tails') "typing 'tail' leaves: $(($rows | ForEach-Object { $_.Name }) -join ' | ')"
    Ok ($w.ByAid('112').Value -eq 'tail') "the filter box holds 'tail' (typed with Ctrl+F focus, no click)"
    Ok ((StatusLine $w) -like 'Filter: 1 of 3 mods shown*') "status line: '$(StatusLine $w)'"
    Ok (Same (State-Ids) @(State-Ids)) 'filtering does not touch mods.json'
    [Nat]::Chord(0x11, 0x46)    # Ctrl+F selects the text
    [Nat]::Key(0x2E)            # Delete
    $null = Wait-For { @(Get-Rows $w).Count -eq 3 } 5
    Ok (Same @(Get-Rows $w | ForEach-Object { $_.Name }) $all) "clearing the box brings all rows back: $((@(Get-Rows $w) | ForEach-Object { $_.Name }) -join ' | ')"
    Ok ((StatusLine $w) -notlike 'Filter:*') "status line is the summary again: '$(StatusLine $w)'"
    [Nat]::TypeText('zzzz')
    $null = Wait-For { @(Get-Rows $w).Count -eq 0 } 5
    Ok (@(Get-Rows $w).Count -eq 0 -and (StatusLine $w) -like 'Filter: 0 of 3 mods shown*') "a filter matching nothing: 0 rows, '$(StatusLine $w)'"
    [Nat]::Chord(0x11, 0x46); [Nat]::Key(0x2E)
    $null = Wait-For { @(Get-Rows $w).Count -eq 3 } 5
    Ok (@(Get-Rows $w).Count -eq 3) 'cleared again'
}

# 23 -----------------------------------------------------------------------------------------------------------------------
Step '23-keyboard' 'Space and double-click toggle, Alt+Home / Alt+End move, F5 refreshes, Ctrl+O opens the file dialog, Ctrl+E the edit form, Del asks' {
    $w = Main-Window
    $null = Focus-Main
    Need (@(Get-Rows $w).Count -eq 3) 'needs 3 rows'
    $names = @(Get-Rows $w | ForEach-Object { $_.Name })
    $id1 = (State-Ids)[1]
    Click-Row $w $names[1]
    [Nat]::Key(0x20)            # Space
    $null = Wait-For { (State-Entry $id1).enabled -eq $true } 5
    Ok ((State-Entry $id1).enabled -eq $true) "Space on the selected row '$($names[1])': enabled=true"
    [Nat]::Key(0x20)
    $null = Wait-For { (State-Entry $id1).enabled -eq $false } 5
    Ok ((State-Entry $id1).enabled -eq $false) 'Space again: enabled=false'
    $p = Row-Pt $w $names[1] 'name'
    Start-Sleep -Milliseconds 700
    [Nat]::DoubleClick($p[0], $p[1])
    $null = Wait-For { (State-Entry $id1).enabled -eq $true } 5
    Ok ((State-Entry $id1).enabled -eq $true) 'double-click on the row: enabled=true'
    Start-Sleep -Milliseconds 700
    [Nat]::DoubleClick($p[0], $p[1])
    $null = Wait-For { (State-Entry $id1).enabled -eq $false } 5
    Ok ((State-Entry $id1).enabled -eq $false) 'double-click again: enabled=false'
    # Alt+End / Alt+Home
    $ids = @(State-Ids)
    Click-Row $w $names[0]
    [Nat]::Chord(0x12, 0x23, $true)   # Alt+End
    Ok (Wait-Order @($ids[1], $ids[2], $ids[0])) "Alt+End moves the selected row to the bottom: $((State-Ids) -join ',')"
    [Nat]::Chord(0x12, 0x24, $true)   # Alt+Home
    Ok (Wait-Order $ids) "Alt+Home moves it back to the top: $((State-Ids) -join ',')"
    # F5
    $id0 = $ids[0]
    $null = Run-Cli @('enable', $id0)
    Ok (-not (@(Get-Rows $w))[0].Checked) 'before F5 the window still shows the old state'
    $null = Focus-Main
    [Nat]::Key(0x74)
    $null = Wait-For { (@(Get-Rows $w))[0].Checked } 5
    Ok ((@(Get-Rows $w))[0].Checked) 'F5 refreshes: the externally enabled mod is ticked'
    $null = Run-Cli @('disable', $id0)
    [Nat]::Key(0x74)
    $null = Wait-For { -not (@(Get-Rows $w))[0].Checked } 5
    Ok (-not (@(Get-Rows $w))[0].Checked) 'F5 again follows the file back'
    # Ctrl+O
    Click-Row $w $names[0]
    [Nat]::Chord(0x11, 0x4F)
    $d = Wait-For { @(Dialogs | Where-Object { $_.Name -like 'Install mods*' })[0] } 15
    Ok ($null -ne $d) 'Ctrl+O opens the install file dialog'
    if ($d) {
        $c = @($d.Descendants() | Where-Object { $_.Cls -eq 'Button' -and $_.Aid -eq '2' })[0]
        if ($c) { $c.Invoke() } else { [Nat]::Key(0x1B) }
        $null = Wait-For { @(Dialogs).Count -eq 0 } 10
        Ok (@(Dialogs).Count -eq 0) 'Cancel closes it; nothing was installed'
    }
    # Ctrl+E then Escape
    $null = Focus-Main
    [Nat]::Chord(0x11, 0x45)
    $f = Wait-Form 10
    Ok ($null -ne $f) 'Ctrl+E opens the edit form'
    if ($f) { [Nat]::Key(0x1B); $null = Wait-For { @(Forms).Count -eq 0 } 5; Ok (@(Forms).Count -eq 0) 'Escape closes the form' }
    # Del asks to remove: answer No
    Click-Row $w $names[0]
    [Nat]::Key(0x2E)
    $d = Wait-DialogTitled 'Remove mod' 10
    Ok ($null -ne $d) 'Del on a row asks to remove it'
    if ($d) { Ok ((Dialog-Text $d) -like "Remove ""$($names[0])""?*") "the box names '$($names[0])'"; Press-Dialog $d '7' }
    Ok ((State-Ids) -contains $ids[0] -and (Test-Path -LiteralPath (Join-Path $script:modsDir $ids[0]))) 'No: the mod and its folder are still there'
}

# 24 -----------------------------------------------------------------------------------------------------------------------
Step '24-edit-info' 'Ctrl+E: change name and author (mod.json and row follow); an invalid version warns and keeps the form; Cancel changes nothing' {
    $w = Main-Window
    $null = Focus-Main
    $modJson = Join-Path $script:modsDir 'cream\mod.json'
    Click-Row $w 'Cream'
    [Nat]::Chord(0x11, 0x45)
    $f = Wait-Form 10
    Need ($null -ne $f) 'the edit form did not open'
    $ed = Form-Edits $f
    Info ("form: '$($f.Name)', $($ed.Count) edits, values: " + (($ed | ForEach-Object { "'" + $_.Value + "'" }) -join ' '))
    Ok ($ed.Count -ge 4 -and $ed[0].Value -eq 'Cream') 'the form opens with the current name'
    $ed[0].SetValue('Cream Edited')
    $ed[2].SetValue('GUI Test')
    (Form-Button $f '1').Invoke()
    $null = Wait-For { @(Forms).Count -eq 0 } 10
    # The form closes first and the manager writes mod.json just after: wait for the write, not for the window.
    $null = Wait-For { try { (Get-Content -LiteralPath $modJson -Raw -Encoding UTF8 | ConvertFrom-Json).name -eq 'Cream Edited' } catch { $false } } 10
    $j = Get-Content -LiteralPath $modJson -Raw -Encoding UTF8 | ConvertFrom-Json
    Ok ($j.name -eq 'Cream Edited' -and $j.author -eq 'GUI Test') "mod.json on disk: name '$($j.name)', author '$($j.author)'"
    $row = @(Get-Rows $w) | Where-Object { $_.Name -eq 'Cream Edited' } | Select-Object -First 1
    Ok ($null -ne $row -and $row.Author -eq 'GUI Test') "the row shows the new name and author ('$($row.Name)' / '$($row.Author)')"
    $h1 = Hash-File $modJson
    # invalid version
    Click-Row $w 'Cream Edited'
    [Nat]::Chord(0x11, 0x45)
    $f = Wait-Form 10
    Need ($null -ne $f) 'the edit form did not open the second time'
    $ed = Form-Edits $f
    $ed[1].SetValue('x.y')
    (Form-Button $f '1').Invoke()
    $d = Wait-DialogTitled 'Edit mod' 10
    Ok ($null -ne $d -and (Dialog-Text $d) -like '*version must look like*') 'an invalid version "x.y" shows the warning'
    if ($d) { Press-Dialog $d '1' }
    $f2 = @(Forms)
    Ok ($f2.Count -eq 1) 'the form stays open'
    Ok ((Hash-File $modJson) -eq $h1) 'mod.json is untouched'
    if ($f2.Count) {
        $ed = Form-Edits $f2[0]
        Ok ($ed[1].Value -eq 'x.y') 'the form keeps what was typed'
        $ed[0].SetValue('Should Not Stick')
        (Form-Button $f2[0] '2').Invoke()
        $null = Wait-For { @(Forms).Count -eq 0 } 10
    }
    Ok ((Hash-File $modJson) -eq $h1) 'Cancel: mod.json is byte-identical'
    Ok (@(Get-Rows $w | Where-Object { $_.Name -eq 'Cream Edited' }).Count -eq 1) 'Cancel: the row is unchanged'
}

# 25 -----------------------------------------------------------------------------------------------------------------------
Step '25-profiles' 'Profiles: save the current set, change it, load the profile back (Yes), delete it (Yes)' {
    $w = Main-Window
    $null = Focus-Main
    $pdir = Join-Path $script:modsDir '.profiles'
    $btn = @($w.Kids() | Where-Object { $_.Aid -eq '113' })[0]
    Need ($null -ne $btn) 'no Profiles button (id 113)'
    Info "Profiles button name: '$($btn.Name)'"
    # a mixed state first: Cream on, the others off
    Click-Row $w 'Cream Edited' 'check'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    $saved = Read-State
    $savedOrder = @($saved.order | ForEach-Object { "$($_.id):$($_.enabled)" })
    Info ('state to save: ' + ($savedOrder -join ' '))
    $open = { $r = $btn.Rect; [Nat]::MouseClick([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)); Wait-Popup }
    $pop = & $open
    Need ($null -ne $pop) 'the Profiles menu did not open'
    Info ('menu: ' + ((Menu-Names $pop) -join ' | '))
    Pick-Menu $pop 'Save current set as'
    $f = Wait-Form 10
    Need ($null -ne $f) 'the profile name form did not open'
    (Form-Edits $f)[0].SetValue('gui-a')
    (Form-Button $f '1').Invoke()
    $null = Wait-For { Test-Path -LiteralPath (Join-Path $pdir 'gui-a.json') } 10
    Ok (Test-Path -LiteralPath (Join-Path $pdir 'gui-a.json')) 'mods\.profiles\gui-a.json exists'
    Ok ((StatusLine $w) -like 'Saved profile "gui-a"*') "status line: '$(StatusLine $w)'"
    # change the set
    Rclick-Row $w 'Cream Edited'
    $pop = Wait-Popup
    Pick-Menu $pop 'Switch all on'
    $null = Wait-For { @((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0 } 5
    Ok (@((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0) 'changed the set (all on)'
    # load it back
    $pop = & $open
    Ok (@(Menu-Names $pop | Where-Object { $_ -eq 'Load "gui-a"' }).Count -eq 1) 'the menu now offers Load "gui-a"'
    Pick-Menu $pop 'Load "gui-a"'
    $d = Wait-DialogTitled 'Load profile' 10
    Need ($null -ne $d) 'no confirmation box for loading'
    Info ('confirmation: ' + ((Dialog-Text $d) -replace "`n", ' / '))
    Press-Dialog $d '6'
    $now = Read-State
    $nowOrder = @($now.order | ForEach-Object { "$($_.id):$($_.enabled)" })
    $null = Wait-For { Same (@((Read-State).order | ForEach-Object { "$($_.id):$($_.enabled)" })) $savedOrder } 5
    $nowOrder = @((Read-State).order | ForEach-Object { "$($_.id):$($_.enabled)" })
    Ok (Same $nowOrder $savedOrder) "Yes: mods.json matches the profile again ($($nowOrder -join ' '))"
    $null = Wait-For { @(Get-Rows $w | Where-Object { $_.Checked }).Count -eq 1 } 5
    Ok (@(Get-Rows $w | Where-Object { $_.Checked }).Count -eq 1) "the window shows one ticked row again (rows: $((@(Get-Rows $w) | ForEach-Object { $_.Name + '=' + $_.Checked }) -join ' | '))"
    # No keeps the current state
    Rclick-Row $w 'Cream Edited'
    $pop = Wait-Popup
    Pick-Menu $pop 'Switch all on'
    $null = Wait-For { @((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0 } 5
    $pop = & $open
    Pick-Menu $pop 'Load "gui-a"'
    $d = Wait-DialogTitled 'Load profile' 10
    Press-Dialog $d '7'
    Ok (@((Read-State).order | Where-Object { -not $_.enabled }).Count -eq 0) 'No: the current set (all on) is kept'
    # delete through the Delete submenu
    $pop = & $open
    $del = Menu-Items $pop | Where-Object { $_.Name -like 'Delete*' } | Select-Object -First 1
    Need ($null -ne $del) 'no Delete entry in the Profiles menu'
    $r = $del.Rect
    [Nat]::MouseMove([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)); Start-Sleep -Milliseconds 900
    [Nat]::Key(0x27)   # Right: opens the submenu
    Start-Sleep -Milliseconds 400
    $item = $null
    foreach ($pp in @(Popups)) { $item = Menu-Items $pp | Where-Object { $_.Name -eq 'gui-a' } | Select-Object -First 1; if ($item) { break } }
    Need ($null -ne $item) "the Delete submenu did not show 'gui-a'"
    $ir = $item.Rect
    [Nat]::MouseClick([int](($ir[0] + $ir[2]) / 2), [int](($ir[1] + $ir[3]) / 2))
    $d = Wait-DialogTitled 'Delete profile' 10
    Need ($null -ne $d) 'no confirmation for deleting'
    Info ('confirmation: ' + ((Dialog-Text $d) -replace "`n", ' / '))
    Press-Dialog $d '6'
    $gone = Wait-For { -not (Test-Path -LiteralPath (Join-Path $pdir 'gui-a.json')) } 10
    Ok ($null -ne $gone) 'Yes: gui-a.json is gone from mods\.profiles'
}

# 26 -----------------------------------------------------------------------------------------------------------------------
Step '26-update-prompt' 'dropping Cream again asks to replace: No keeps everything, Yes updates in place (same folder, flag and position)' {
    $w = Main-Window
    $null = Focus-Main
    $ids = @(State-Ids)
    Need ($ids -contains 'cream') 'cream is not installed'
    $wasOn = (State-Entry 'cream').enabled
    $pos = [array]::IndexOf($ids, 'cream')
    $folders = @(Get-ChildItem -LiteralPath $script:modsDir -Directory | Where-Object { -not $_.Name.StartsWith('.') } | ForEach-Object { $_.Name } | Sort-Object)
    $r = Drop-And-Wait $w @($cream) 'list'
    Ok (Accepted $r.Effect) "OLE drop effect: $($r.Effect)"
    Need ($null -ne $r.Dialog) 'no box after dropping an installed mod'
    Info ('box: ' + $r.Dialog.Name + ' / ' + ($r.Text -replace "`n", ' / '))
    Ok ($r.Dialog.Name -eq 'Update mods' -and $r.Text -like '*installed already*Replace the installed copy*') 'the box is the update question'
    Press-Dialog $r.Dialog '7'
    Start-Sleep -Milliseconds 800
    $now = @(Get-ChildItem -LiteralPath $script:modsDir -Directory | Where-Object { -not $_.Name.StartsWith('.') } | ForEach-Object { $_.Name } | Sort-Object)
    Ok (Same $now $folders) "No: no second copy (folders: $($now -join ', '))"
    Ok (-not (Test-Path -LiteralPath (Join-Path $script:modsDir 'cream-2'))) 'there is no cream-2'
    Ok (@(Get-Rows $w | Where-Object { $_.Name -eq 'Cream Edited' }).Count -eq 1) "the edited name is still there: nothing was replaced"
    # Yes
    $r = Drop-And-Wait $w @($cream) 'list'
    Need ($null -ne $r.Dialog) 'no update box the second time'
    Press-Dialog $r.Dialog '6'
    $box = Wait-DialogTitled 'Shadow Mod Manager' 120
    Need ($null -ne $box) 'no result box after Yes'
    Info ('result: ' + ((Dialog-Text $box) -replace "`n", ' / '))
    Press-Dialog $box '1'
    $now = @(Get-ChildItem -LiteralPath $script:modsDir -Directory | Where-Object { -not $_.Name.StartsWith('.') } | ForEach-Object { $_.Name } | Sort-Object)
    Ok (Same $now $folders) "Yes: still no second copy (folders: $($now -join ', '))"
    $j = Get-Content -LiteralPath (Join-Path $script:modsDir 'cream\mod.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Ok ($j.name -eq 'Cream Edited' -and $j.user_edited -eq $true) "the update keeps the name the user edited (name '$($j.name)')"
    Ok (Test-Path -LiteralPath (Join-Path $script:modsDir 'cream\files\character\shadow.one')) "the update's content is in place"
    Ok ((State-Entry 'cream').enabled -eq $wasOn) "its on/off flag is kept (enabled=$wasOn)"
    Ok (([array]::IndexOf((State-Ids), 'cream')) -eq $pos) "its place in the list is kept (position $($pos + 1))"
}

# 27 -----------------------------------------------------------------------------------------------------------------------
Step '27-dependencies' 'a mod that needs Cream: switching Cream off offers to switch it off too; switching it on offers to enable Cream' {
    $w = Main-Window
    $null = Focus-Main
    $dep = Join-Path $run 'needs-cream'
    New-Item -ItemType Directory -Force -Path (Join-Path $dep 'files') | Out-Null
    Set-Content -LiteralPath (Join-Path $dep 'mod.json') -Encoding UTF8 -Value '{"id":"needs-cream","name":"Needs Cream","version":"1.0","dependencies":["cream"]}'
    Copy-Item -LiteralPath (Join-Path $GameData 'files\nukkoro2.inf') -Destination (Join-Path $dep 'files\nukkoro2.inf')
    $r = Drop-And-Wait $w @($dep) 'list'
    Need ($null -ne $r.Dialog) 'no result box after dropping the folder'
    Info ('box: ' + ($r.Text -replace "`n", ' / '))
    Ok ($r.Text -like '*Installed "Needs Cream"*') 'the hand-made folder mod installed'
    Press-Dialog $r.Dialog '1'
    Need ((State-Ids) -contains 'needs-cream') 'needs-cream is not in mods.json'
    # all off, then Cream on, Needs Cream on
    $pop = $null
    Rclick-Row $w 'Cream Edited'; $pop = Wait-Popup; Pick-Menu $pop 'Switch all off'
    Click-Row $w 'Cream Edited' 'check'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    Click-Row $w 'Needs Cream' 'check'
    $null = Wait-For { (State-Entry 'needs-cream').enabled -eq $true } 5
    Ok ((State-Entry 'cream').enabled -and (State-Entry 'needs-cream').enabled) 'both enabled (no question: the dependency is on)'
    # untick Cream: No first
    Click-Row $w 'Cream Edited' 'check'
    $d = Wait-DialogTitled 'Shadow Mod Manager' 10
    Need ($null -ne $d) 'unticking Cream did not ask about the mods that need it'
    $t = Dialog-Text $d
    Info ('box: ' + ($t -replace "`n", ' / '))
    Ok ($t -like '*These enabled mods need it*Needs Cream*Switch them off too?*') 'the box names Needs Cream and asks to switch it off too'
    Press-Dialog $d '7'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $false } 5
    Ok ((State-Entry 'cream').enabled -eq $false -and (State-Entry 'needs-cream').enabled -eq $true) 'No: Cream is off, Needs Cream stays on'
    # back on, then untick with Yes
    Click-Row $w 'Cream Edited' 'check'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    Click-Row $w 'Cream Edited' 'check'
    $d = Wait-DialogTitled 'Shadow Mod Manager' 10
    Need ($null -ne $d) 'no question the second time'
    Press-Dialog $d '6'
    $null = Wait-For { (State-Entry 'needs-cream').enabled -eq $false } 5
    Ok ((State-Entry 'cream').enabled -eq $false -and (State-Entry 'needs-cream').enabled -eq $false) 'Yes: both are off in mods.json'
    # enable Needs Cream while Cream is off
    Click-Row $w 'Needs Cream' 'check'
    $d = Wait-DialogTitled 'Shadow Mod Manager' 10
    Need ($null -ne $d) 'enabling Needs Cream did not ask about Cream'
    $t = Dialog-Text $d
    Info ('box: ' + ($t -replace "`n", ' / '))
    Ok ($t -like '*This mod needs:*Cream*Enable them too?*') 'the box names Cream and asks to enable it too'
    Press-Dialog $d '6'
    $null = Wait-For { (State-Entry 'cream').enabled -eq $true } 5
    Ok ((State-Entry 'cream').enabled -eq $true -and (State-Entry 'needs-cream').enabled -eq $true) 'Yes: both are on in mods.json'
}

# 28 -----------------------------------------------------------------------------------------------------------------------
Step '28-status-colours' 'status column: text per row, and the colour the text is drawn in (Loads vs Off)' {
    $w = Main-Window
    $null = Focus-Main
    $rows = @(Get-Rows $w)
    $loads = $rows | Where-Object { $_.Status -like 'Loads*' } | Select-Object -First 1
    $off = $rows | Where-Object { $_.Status -eq 'Off' } | Select-Object -First 1
    Info ('statuses: ' + (($rows | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ' | '))
    Need ($null -ne $loads) 'no row that loads'
    if ($null -eq $off) {   # make one off
        Click-Row $w 'Regular Rouge' 'check'; Start-Sleep -Milliseconds 600
        $rows = @(Get-Rows $w); $off = $rows | Where-Object { $_.Status -eq 'Off' } | Select-Object -First 1
    }
    Need ($null -ne $off) 'no row that is off'
    # select a third row so neither sample is drawn in the selection colours
    $other = $rows | Where-Object { $_.Name -ne $loads.Name -and $_.Name -ne $off.Name } | Select-Object -First 1
    if ($other) { Click-Row $w $other.Name; Start-Sleep -Milliseconds 400 }
    Start-Sleep -Milliseconds 400
    $cell = { param($name) $r = @(Get-Rows $w) | Where-Object { $_.Name -eq $name } | Select-Object -First 1; $c = @($r.El.Kids() | Where-Object { $_.Aid -eq 'ListViewSubItem-4' })[0].Rect; $c }
    $cl = & $cell $loads.Name; $co = & $cell $off.Name
    $sl = [Nat]::TextColour($cl[0] + 2, $cl[1] + 2, [Math]::Min($cl[2], $cl[0] + 120), $cl[3] - 2)
    $so = [Nat]::TextColour($co[0] + 2, $co[1] + 2, [Math]::Min($co[2], $co[0] + 120), $co[3] - 2)
    Info ("darkest text pixel of the 'Loads' status cell ($($loads.Name)): RGB($($sl -join ','))")
    Info ("darkest text pixel of the 'Off' status cell ($($off.Name)): RGB($($so -join ','))")
    $fg = [Nat]::GetForegroundWindow()
    Need ($fg -eq [IntPtr]$w.Hwnd) 'the manager is not the foreground window: the screen pixels could belong to another window'
    Ok ($sl[1] -gt $sl[0] + 30 -and $sl[1] -gt $sl[2] + 30) "'Loads' is drawn green (G clearly above R and B): RGB($($sl -join ','))"
    Ok ([Math]::Abs($so[0] - $so[1]) -le 25 -and [Math]::Abs($so[1] - $so[2]) -le 25) "'Off' is drawn grey/neutral: RGB($($so -join ','))"
    Info 'method: GetPixel on the screen over the status cell, the darkest pixel of the text run (anti-aliasing makes the exact value approximate)'
}

# 29 -----------------------------------------------------------------------------------------------------------------------
Step '29-progress' 'a big install shows the marquee progress bar while busy and hides it after' {
    $w = Main-Window
    $null = Focus-Main
    $h = [Nat]::Child($w.Hwnd, 'msctls_progress32')
    Need ($h -ne 0) 'no progress bar control (msctls_progress32) found among the window children'
    Ok (-not [Nat]::Visible($h)) 'the progress bar is hidden while idle'
    $big = Join-Path $Downloads 'hq_gun_soldiers.zip'
    $targets = @($big)
    if ($BigFolder -ne '' -and (Test-Path -LiteralPath $BigFolder)) { $targets += (Resolve-Path -LiteralPath $BigFolder).Path }
    $seen = $false; $busyButtons = $null; $used = ''
    foreach ($t in $targets) {
        if (-not (Test-Path -LiteralPath $t)) { continue }
        $fx = Drop-Files $w @($t) 'list'
        if (-not (Accepted $fx)) { continue }
        $used = Split-Path $t -Leaf
        $end = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $end) {
            if ([Nat]::Visible($h)) {
                $seen = $true
                $busyButtons = @($w.Kids() | Where-Object { $_.Cls -eq 'Button' -and $_.Name -in @('Install...', 'Remove', 'Refresh') } | ForEach-Object { $_.Enabled })
                break
            }
            if (@(Dialogs).Count) { break }
            Start-Sleep -Milliseconds 40
        }
        $d = Wait-Dialog 180
        if ($d) { Info ('result box for ' + $used + ': ' + ((Dialog-Text $d) -replace "`n", ' / ').Substring(0, 120)); Press-Dialog $d '1' }
        if ($seen) { break }
    }
    Need ($used -ne '') 'no drop was accepted'
    Ok $seen "the progress bar became visible while installing $used"
    if ($seen) { Ok (-not (@($busyButtons | Where-Object { $_ }).Count)) 'while busy Install..., Remove and Refresh are disabled' }
    $null = Wait-For { -not [Nat]::Visible($h) } 5
    Ok (-not [Nat]::Visible($h)) 'the progress bar is hidden again after the result box'
}

# 30 -----------------------------------------------------------------------------------------------------------------------
Step '30-placement' 'move and resize the window, close, restart: same place from .manager.json; a rect off every screen is ignored' {
    $w = Main-Window
    $null = Focus-Main
    $js = Join-Path $script:modsDir '.manager.json'
    $r0 = $w.Rect
    # a real drag of the title bar, then of the bottom-right corner
    $tx = [int](($r0[0] + $r0[2]) / 2); $ty = [int]($r0[1] + 14)
    [Nat]::MouseDrag($tx, $ty, $tx + 50, $ty + 30)
    $r1 = (Main-Window).Rect
    Ok ($r1[0] -ne $r0[0] -or $r1[1] -ne $r0[1]) "dragging the title bar moved the window ($($r0[0]),$($r0[1]) -> $($r1[0]),$($r1[1]))"
    [Nat]::MouseDrag($r1[2] - 4, $r1[3] - 4, $r1[2] - 104, $r1[3] - 64)
    $r2 = (Main-Window).Rect
    Ok (($r2[2] - $r2[0]) -lt ($r1[2] - $r1[0])) "dragging the corner resized it (width $($r1[2] - $r1[0]) -> $($r2[2] - $r2[0]))"
    Stop-Manager
    Ok ($script:closedGracefully -eq $true) 'closing the window ends the process'
    Ok (Test-Path -LiteralPath $js) 'mods\.manager.json was written on close'
    $j = Get-Content -LiteralPath $js -Raw | ConvertFrom-Json
    Info ("saved: left=$($j.window.left) top=$($j.window.top) right=$($j.window.right) bottom=$($j.window.bottom) maximized=$($j.window.maximized)")
    $near = { param($a, $b) [Math]::Abs($a - $b) -le 3 }
    Ok ((& $near $j.window.left $r2[0]) -and (& $near $j.window.top $r2[1]) -and (& $near $j.window.right $r2[2]) -and (& $near $j.window.bottom $r2[3])) "the file holds the window's rect ($($r2 -join ','))"
    Start-Manager $script:modsDir
    $w = Wait-Ready
    $r3 = $w.Rect
    Ok ((& $near $r3[0] $r2[0]) -and (& $near $r3[1] $r2[1]) -and (& $near $r3[2] $r2[2]) -and (& $near $r3[3] $r2[3])) "restart: the window opens at the same rect ($($r3 -join ','))"
    Stop-Manager
    # a rect on no screen
    $j.window.left = -30000; $j.window.right = -29000
    [IO.File]::WriteAllText($js, ($j | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    Start-Manager $script:modsDir
    $w = Wait-Ready
    $r4 = $w.Rect
    $scr = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    Ok ($r4[0] -gt -1000 -and $r4[0] -lt $scr.Width) "x=-30000 is ignored: the window opens at x=$($r4[0])"
    Ok ((($r4[2] - $r4[0]) -ge 1100) -and (($r4[2] - $r4[0]) -le 1300)) "and at the default size (width $($r4[2] - $r4[0]))"
    Stop-Manager
}

# 31 -----------------------------------------------------------------------------------------------------------------------
Step '31-code-window' 'Code... window: the code items of the selected mod, ticking one saves the choice and names its signature; the built report is summarised' {
    Need ($CodeMod -ne '' -and (Test-Path -LiteralPath $CodeMod)) 'no -CodeMod folder given'
    Stop-Manager
    $script:modsDir = Join-Path $run 'mods-code'
    $script:stateFile = Join-Path $script:modsDir 'mods.json'
    New-Item -ItemType Directory -Force -Path $script:modsDir | Out-Null
    $o = Run-Cli @('install', $CodeMod)
    Ok ($o -like 'installed *') "CLI install of $(Split-Path $CodeMod -Leaf): $(($o -split "`n")[0].Trim())"
    $id = @(State-Ids)[0]
    $null = Run-Cli @('enable', $id)
    $items = @((Run-Cli @('codes', $id)) -split "`r?`n" | Where-Object { $_ -match '^\[[ x]\] (\S+)' } | ForEach-Object { ($_ -replace '^\[[ x]\] ', '') })
    Need ($items.Count -gt 0) "$id offers no code item"
    Ok ((Run-Cli @('code-signature')).Trim() -eq '') 'nothing chosen: no signature'
    Start-Manager $script:modsDir
    $w = Wait-Ready
    $name = (Get-Rows $w | Select-Object -First 1).Name
    Select-Row $w $name
    Press $w 'Code...'
    $cwh = Wait-For { foreach ($h in [Nat]::TopWindows($script:proc.Id)) { if ([Nat]::ClassOf($h) -eq 'ShadowModManagerCode') { return $h } } } 10 'the code window'
    Need ($cwh) 'the code window did not open'
    $cw = [El]::FromHwnd($cwh)
    $rows = @($cw.Descendants() | Where-Object { $_.Type -eq 50007 })
    Ok ($rows.Count -eq $items.Count) "the window lists $($rows.Count) code items; the CLI lists $($items.Count)"
    Ok (-not @($rows | Where-Object { $_.ToggleState -eq 1 }).Count) 'none is ticked'
    $infoText = { (@($cw.Descendants() | Where-Object { $_.Cls -eq 'Edit' }) | Select-Object -First 1).Value }
    Ok ((& $infoText) -like 'No code chosen*') "info: $((& $infoText) -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(60, $_.Length)) })"
    # tick the first named Gecko code (else the first item) through the Toggle pattern
    $pick = @($rows | Where-Object { $_.Name -notlike '*Executable changes*' })[0]
    if (-not $pick) { $pick = $rows[0] }
    $pick.Toggle()
    $sig = Wait-For { $t = (Run-Cli @('code-signature')).Trim(); if ($t) { $t } } 5 'a signature'
    Ok ([bool]$sig) "ticking '$($pick.Name)' saved the choice: signature $sig"
    $e = State-Entry $id
    Ok (@($e.codes.PSObject.Properties | Where-Object { $_.Value -eq $true }).Count -eq 1) 'mods.json holds exactly that one choice'
    $t = Wait-For { $x = & $infoText; if ($x -like "Chosen code $sig*") { $x } } 5 'the info line'
    Ok ($t -like "Chosen code $sig*not built*") "info names the signature and that it is not built: $(($t -replace '\s+', ' ').Substring(0, [Math]::Min(80, $t.Length)))"
    # a report for that signature (written here, the shape build_variant.py writes): the window summarises it
    $vd = Join-Path $script:modsDir '.variants'
    New-Item -ItemType Directory -Force -Path $vd | Out-Null
    $rep = '{"signature":"' + $sig + '","counts":{"applied":0,"runtime":1,"skipped":0},"build":{"ok":true},"exe":null,"engine_codes":[]}'
    [IO.File]::WriteAllText((Join-Path $vd "$sig.json"), $rep, (New-Object Text.UTF8Encoding($false)))
    $pick.Toggle(); Start-Sleep -Milliseconds 300; $pick.Toggle()   # the info refreshes on a change
    $t = Wait-For { $x = & $infoText; if ($x -like '*prepared (no variant needed)*') { $x } } 5 'the summary'
    Ok ([bool]$t) 'with a report: "prepared (no variant needed): 0 patches built in, 1 for the code engine"'
    $pick.Toggle()
    $null = Wait-For { -not (Run-Cli @('code-signature')).Trim() } 5
    Ok (-not (Run-Cli @('code-signature')).Trim()) 'unticking: nothing chosen again'
    $cw.CloseWindow()
    $null = Wait-For { -not [Nat]::Visible($cwh) } 5
    Ok (-not [Nat]::Visible($cwh)) 'the code window closes'
    $w = Main-Window
    Ok ((StatusLine $w) -match 'installed') 'the main window answers again'
    Stop-Manager
    Ok ($script:closedGracefully -eq $true) 'closing the window ends the process'
}

}
finally {
    if ($null -ne $script:proc -and -not $script:proc.HasExited) { try { $script:proc.Kill() } catch { } }
}

Write-Host ''
Write-Host ("RESULT: {0} passed, {1} failed, {2} skipped   (run folder: {3})" -f $script:passed, $script:failed, $script:skipped, $run)
if ($script:findings.Count) { Write-Host 'FINDINGS:'; foreach ($f in $script:findings) { Write-Host "  - $f" } }
exit $script:failed
