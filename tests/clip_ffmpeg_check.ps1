# OPTIONAL end-to-end check of the clipping buffer's ffmpeg command lines (geode v0.6.0, stream M3).
# Not part of run_tests.ps1: it needs a real ffmpeg.exe and launches it (no window, a few seconds).
#
# It runs, OUTSIDE Geometry Dash, exactly the command lines core/clip builds for the mod:
#   1. the encoder self-tests (probeArgs) for h264_nvenc / h264_amf / h264_qsv / libx264;
#   2. for every encoder that works: the rolling segmenter (segmenterArgs) fed with synthetic raw
#      BGRA frames of an 1850x1041 window through a pipe, like the mod's writer thread does;
#   3. the segment list -> BufferBook -> select -> ffconcat list (clip_args_tool select);
#   4. the clip mux (muxArgs: video stream copy + a game and a microphone sound track);
#   5. checks: 1-second segments listed, the clip decodes without errors, is H.264 at the expected
#      size with two AAC tracks and the expected length, and core/clip_flow hashFile equals
#      Get-FileHash.
# What it cannot check (needs the game): the GL read-back, the FMOD / WASAPI taps, the popup.
#
#   -Ffmpeg path   ffmpeg.exe (default: this mod's save folder, the In-Game Clipper's, then PATH)
#   -OutDir dir    work folder (default: geode\build\clip-check, wiped first)
#   -Quality q     480p | 720p | 1080p (default 720p)
# Exit code: 0 = every check passed (or SKIPPED: no ffmpeg found), 1 = a check failed.
param([string]$Ffmpeg = '', [string]$OutDir = '', [string]$Quality = '720p')
$ErrorActionPreference = 'Continue'
. 'D:\GeodeMods\_msvc\msvc-env.ps1'
$geode = Split-Path $PSScriptRoot -Parent

if (-not $Ffmpeg) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'GeometryDash\geode\mods\gmo12.gprl\ffmpeg.exe'),
        (Join-Path $env:LOCALAPPDATA 'GeometryDash\geode\mods\gmo12.ingame_clipper\ffmpeg.exe')
    )
    foreach ($c in $candidates) { if (-not $Ffmpeg -and (Test-Path $c)) { $Ffmpeg = $c } }
    if (-not $Ffmpeg) {
        $cmd = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
        if ($cmd) { $Ffmpeg = $cmd.Source }
    }
}
if (-not $Ffmpeg -or -not (Test-Path $Ffmpeg)) {
    Write-Output 'clip_ffmpeg_check: SKIPPED (no ffmpeg.exe found; pass -Ffmpeg <path>)'
    exit 0
}

$out = if ($OutDir) { $OutDir } else { Join-Path $geode 'build\clip-check' }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Set-Location $out
Write-Output "clip_ffmpeg_check: ffmpeg $Ffmpeg, quality $Quality, work folder $out"

cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 "$geode\tests\clip_args_tool.cpp" "$geode\core\clip.cpp" "$geode\core\clip_flow.cpp" `
    "$geode\core\crypto.cpp" "$geode\core\json.cpp" /Fe:clip_args_tool.exe | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Output 'clip_ffmpeg_check: clip_args_tool compile failed'
    exit 1
}

$script:failures = 0
$script:checks = 0
function Check($ok, $what) {
    $script:checks++
    if (-not $ok) {
        $script:failures++
        Write-Output "  FAIL $what"
    }
}

# Runs one command line through a batch file: cmd pipes are binary safe (PowerShell 5.1's are not)
# and the quoting of the ffmpeg arguments stays exactly as the mod passes it to CreateProcess.
function Run-Line($line, $log) {
    $bat = Join-Path $out 'run.cmd'
    Set-Content -Path $bat -Value ("@echo off`r`n" + $line.Replace('%', '%%') + "`r`n") -Encoding ASCII
    & cmd.exe /c $bat > $log 2>&1
    return $LASTEXITCODE
}

# ---- 1. encoder self-tests ----
$working = @()
foreach ($e in 'h264_nvenc', 'h264_amf', 'h264_qsv', 'libx264') {
    $probe = & .\clip_args_tool.exe probe $e $Quality
    $rc = Run-Line "`"$Ffmpeg`" $probe" (Join-Path $out "probe-$e.log")
    Write-Output ("- probe {0}: {1}" -f $e, $(if ($rc -eq 0) { 'works' } else { "not usable (exit $rc)" }))
    if ($rc -eq 0) { $working += $e }
}
Check ($working.Count -gt 0) 'no H.264 encoder works with this ffmpeg'

$size = @{ '480p' = '854x480'; '720p' = '1280x720'; '1080p' = '1850x1040' }[$Quality]

foreach ($e in $working) {
    Write-Output "- ${e}: segmenter -> select -> mux -> verify"
    $dir = Join-Path $out "s-$e"
    New-Item -ItemType Directory -Force (Join-Path $dir 'work') | Out-Null

    # ---- 2. the rolling segmenter, raw BGRA frames on stdin ----
    $seg = & .\clip_args_tool.exe segment 1850 1041 $Quality $e "$dir\segments.csv" "$dir\seg_%05d.mkv" 2>$null
    $gen = "`"$Ffmpeg`" -hide_banner -loglevel error -f lavfi -i testsrc2=s=1850x1041:r=60 -frames:v 390 -f rawvideo -pix_fmt bgra -"
    $rc = Run-Line "$gen | `"$Ffmpeg`" $seg" (Join-Path $dir 'segment.log')
    Check ($rc -eq 0) "$e segmenter exit $rc"
    $csv = @(Get-Content (Join-Path $dir 'segments.csv') -ErrorAction SilentlyContinue)
    Check ($csv.Count -eq 7) "$e segment list has $($csv.Count) lines, expected 7 (6.5 s of frames)"
    Check ($csv.Count -gt 0 -and $csv[0] -eq 'seg_00000.mkv,0.000000,1.000000') "$e first segment line is '$($csv[0])'"

    # ---- 3. segment list -> BufferBook -> select -> ffconcat ----
    $sel = & .\clip_args_tool.exe select $dir 1000 1000.4 1005.2 "$dir\work\source.ffconcat"
    Check ($sel -match '^picks 6 duration 6\.000000 start 1000\.000000 end 1006\.000000 coversStart 1 ') "$e select said '$sel'"

    # ---- 4. the clip mux: stream copy + two sound tracks ----
    Run-Line "`"$Ffmpeg`" -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=6 -ac 2 -c:a pcm_f32le `"$dir\work\game.wav`"" (Join-Path $dir 'wav.log') | Out-Null
    Run-Line "`"$Ffmpeg`" -hide_banner -loglevel error -y -f lavfi -i sine=frequency=880:sample_rate=48000:duration=6 -ac 1 -c:a pcm_s16le `"$dir\work\mic.wav`"" (Join-Path $dir 'wav.log') | Out-Null
    $mux = & .\clip_args_tool.exe mux "$dir\work\source.ffconcat" "$dir\work\game.wav" "$dir\work\mic.wav" "$dir\clip.mp4" 'clip-19a0b1c2d3e-5f6a7b8c' 's1-a7' 'GPRL Test Level 100 percent attempt 7'
    $rc = Run-Line "`"$Ffmpeg`" $mux" (Join-Path $dir 'mux.log')
    Check ($rc -eq 0) "$e mux exit $rc"
    Check (Test-Path "$dir\clip.mp4") "$e clip.mp4 missing"
    if (-not (Test-Path "$dir\clip.mp4")) { continue }

    # ---- 5. the clip is what the mod says it is ----
    $rc = Run-Line "`"$Ffmpeg`" -hide_banner -v error -i `"$dir\clip.mp4`" -f null -" (Join-Path $dir 'decode.log')
    $decodeLog = (Get-Content (Join-Path $dir 'decode.log') -Raw -ErrorAction SilentlyContinue)
    Check ($rc -eq 0 -and [string]::IsNullOrWhiteSpace($decodeLog)) "$e clip does not decode cleanly: $decodeLog"
    Run-Line "`"$Ffmpeg`" -hide_banner -i `"$dir\clip.mp4`"" (Join-Path $dir 'info.log') | Out-Null
    $info = Get-Content (Join-Path $dir 'info.log') -Raw
    Check ($info -match 'Duration: 00:00:06\.0') "$e clip duration is not 6.0 s"
    # (?s): the redirected ffmpeg banner is wrapped, the size may sit on the next line
    Check ($info -match "(?s)Video: h264 .{0,200}?$size") "$e video stream is not H.264 $size"
    Check (([regex]::Matches($info, 'Audio: aac')).Count -eq 2) "$e clip does not have two AAC tracks"
    Check ($info -match 'handler_name\s*:\s*Game' -and $info -match 'handler_name\s*:\s*Microphone') "$e sound tracks are not named Game / Microphone"
    Check ($info -match 'comment\s*:\s*gprl clip clip-19a0b1c2d3e-5f6a7b8c attempt s1-a7') "$e clip / attempt ids missing from the metadata"
    $hash = (& .\clip_args_tool.exe hash "$dir\clip.mp4") -split ' '
    $real = (Get-FileHash -Algorithm SHA256 "$dir\clip.mp4").Hash.ToLower()
    Check ($hash[0] -eq $real) "$e hashFile $($hash[0]) != Get-FileHash $real"
    Check ([int64]$hash[1] -eq (Get-Item "$dir\clip.mp4").Length) "$e hashFile size $($hash[1])"
    $kbps = [math]::Round((Get-Item "$dir\clip.mp4").Length * 8 / 6 / 1000)
    Write-Output "  clip.mp4: $((Get-Item "$dir\clip.mp4").Length) bytes (~$kbps kbit/s), sha256 $($hash[0].Substring(0, 12))..."
}

Write-Output "clip_ffmpeg_check: $($script:checks) checks, $($script:failures) failures (encoders that work: $($working -join ', '))"
if ($script:failures -eq 0) { Write-Output 'CLIP FFMPEG CHECK PASSED'; exit 0 }
Write-Output 'CLIP FFMPEG CHECK FAILED'
exit 1
