# Captures the main window of a running process to a PNG (PrintWindow with
# PW_RENDERFULLCONTENT, so GPU-composited content is included).
# Note: find windows via (Get-Process ...).MainWindowHandle; FindWindowW
# returns 0 in this tooling environment.
param([int]$ProcessId, [string]$OutFile)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win -Name Cap2 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$p = Get-Process -Id $ProcessId
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw 'no window' }
[Win.Cap2]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
$rect = New-Object Win.Cap2+RECT
[Win.Cap2]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win.Cap2]::PrintWindow($hwnd, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $OutFile"
