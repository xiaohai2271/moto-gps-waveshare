#!/usr/bin/env pwsh
# shot_device.ps1 - Use the Android phone camera (via adb) to photograph the
# round device screen, then crop/zoom it so the picture can be inspected.
#
# Usage (from the repo root):
#   pwsh -ExecutionPolicy Bypass -File scripts\shot_device.ps1
#
# Optional parameters:
#   -Serial <id>     adb serial (default: first non-emulator device)
#   -OutDir <dir>    output directory   (default: build/dbg)
#   -CenterX/-CenterY/-RadiusRatio  device position in the photo frame
#
# Outputs in OutDir:
#   device_full.jpg     full camera photo pulled from the phone
#   device_screen.jpg   cropped + 2x zoomed device screen
#   device_overview.jpg downscaled whole photo for orientation
#
# The phone camera app is launched and the shutter is triggered here, so aim
# the camera at the device before running.

param(
    [string]$Serial = "",
    [string]$OutDir = "build/dbg",
    [double]$CenterX = 0.394,
    [double]$CenterY = 0.442,
    [double]$RadiusRatio = 0.115,
    [int]$WaitsMs = 1800
)

$ErrorActionPreference = "Stop"

function Find-Adb {
    $candidates = @(
        "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
        "C:\Users\zheng\AppData\Local\Android\Sdk\platform-tools\adb.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "adb.exe not found. Install Android platform-tools or add adb to PATH."
}

$adb = Find-Adb
$py = "C:\Users\zheng\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
if (-not (Test-Path $py)) { throw "Bundled python not found at $py" }

# adb writes progress to stderr; PowerShell turns that into a red error record
# whenever stderr is redirected. Route stderr to a file for every call.
$ErrFile = Join-Path $env:TEMP "shot_device_adb_err.txt"

if (-not $Serial) {
    $raw = (& $adb devices 2>&1) -join "`n"
    foreach ($line in ($raw -split "`r?`n")) {
        if ($line -match "^([A-Za-z0-9.-]+)\s+device") {
            $id = $Matches[1]
            if ($id -notlike "emulator*") { $Serial = $id; break }
        }
    }
    if (-not $Serial) { throw "No physical Android device found over adb." }
}
Write-Host "Using adb serial: $Serial"

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# 1) Open the still camera and take two shots (the first can be blurry).
& $adb -s $Serial shell am start -a android.media.action.STILL_IMAGE_CAMERA 2>&1 | Out-Null
Start-Sleep -Seconds ([int]($WaitsMs / 1000))
& $adb -s $Serial shell input keyevent KEYCODE_CAMERA 2>&1 | Out-Null
Start-Sleep -Seconds 2
& $adb -s $Serial shell input keyevent KEYCODE_CAMERA 2>&1 | Out-Null
Start-Sleep -Seconds 2

# 2) Find the newest photo in the camera roll.
$latest = (((& $adb -s $Serial shell "ls -t /sdcard/DCIM/Camera/*.jpg 2>/dev/null | head -1" 2>&1) -join "`n")).Trim()
if (-not $latest) { throw "No photo found in /sdcard/DCIM/Camera" }
Write-Host "Newest photo on phone: $latest"

$full = Join-Path $OutDir "device_full.jpg"
# cmd /c keeps adb's chatty stderr out of the PowerShell pipeline entirely.
cmd /c "`"$adb`" -s $Serial pull `"$latest`" `"$full`" 2>&1" | Out-Null
if (-not (Test-Path $full)) { throw "adb pull failed for $latest" }
Write-Host ("Pulled: {0} ({1} MB)" -f $full, [math]::Round((Get-Item $full).Length / 1MB, 1))

# 3) Crop the device screen out and zoom it, plus a small overview. The crop
#    parameters are fractions of the photo, so they survive resolution changes.
$pyCode = "from PIL import Image`n" +
          "import sys`n" +
          "full, outdir, cx, cy, r = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])`n" +
          "im = Image.open(full)`n" +
          "w, h = im.size`n" +
          "ov = im.copy(); ov.thumbnail((900, 1200)); ov.save(outdir + '/device_overview.jpg', quality=88)`n" +
          "X, Y = int(cx * w), int(cy * h)`n" +
          "R = int(r * w)`n" +
          "box = (max(0, X - R), max(0, Y - R), min(w, X + R), min(h, Y + R))`n" +
          "crop = im.crop(box)`n" +
          "crop = crop.resize((crop.size[0] * 2, crop.size[1] * 2), Image.LANCZOS)`n" +
          "crop.save(outdir + '/device_screen.jpg', quality=92)`n" +
          "print('screen=%dx%d full=%dx%d' % (crop.size[0], crop.size[1], w, h))`n"
$codePath = Join-Path $OutDir "_crop.py"
[System.IO.File]::WriteAllText($codePath, $pyCode)
& $py $codePath $full $OutDir $CenterX $CenterY $RadiusRatio

Write-Host ""
Write-Host "Done. Open:"
Write-Host "  $OutDir\device_screen.jpg   <- device screen (cropped/zoomed)"
Write-Host "  $OutDir\device_overview.jpg <- whole photo"
