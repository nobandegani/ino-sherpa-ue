# setup-sherpa-onnx.ps1
#
# Idempotent setup for the InoSherpa plugin -- Phase 1: Win64 only.
#
#   Win64 (prebuilt static download)
#     Downloads upstream's official
#         sherpa-onnx-v<ver>-win-x64-static-MD-Release-lib.tar.bz2
#     and stages all 14 static .lib files (sherpa + its bundled CPU-only
#     ONNX Runtime + espeak-ng/kaldi/piper deps) to Source/ThirdParty/Win64/.
#     No build toolchain required. The libs are linked INTO the InoSherpa
#     module DLL -- no sherpa-shipped runtime binary (and no plain-named
#     onnxruntime.dll) ever exists at runtime. That's the whole strategy:
#     see CLAUDE.md "The one core decision: static everywhere".
#
#     MD-Release matches UE's /MD dynamic CRT. Debug/RelWithDebInfo
#     variants exist upstream if ever needed for lib-level debugging.
#
#   Android arm64-v8a (phase 2 -- NOT YET STAGED)
#     Will be built from the Vendor/sherpa-onnx submodule with
#     SHERPA_ONNX_ENABLE_C_API=ON, SHERPA_ONNX_ENABLE_JNI=OFF and ORT
#     linked statically (upstream publishes no usable static Android
#     release -- their static-link artifact is JNI-only). See CLAUDE.md
#     "Why Android is self-built".
#
#   iOS / macOS (phase 3 -- NOT YET STAGED)
#     Official static release artifacts (ios.tar.bz2 xcframework /
#     osx-*-static packages).
#
# Headers are NOT staged: the C API header is consumed straight from the
# Vendor/sherpa-onnx submodule via PrivateIncludePaths (the -lib release
# archives ship no headers). That is why the submodule MUST be checked
# out at the exact tag matching SHERPA_VERSION -- this script enforces it.
#
# Pinned version lives in:
#   SherpaOnnx/SHERPA_VERSION                    (e.g. "1.13.3")
#   Vendor/sherpa-onnx                           (git submodule, MUST match tag v<ver>)
# Bump SHERPA_VERSION + `git -C Vendor/sherpa-onnx checkout v<ver>` to update.
#
# Idempotency stamp: Source/ThirdParty/.sherpa_version. If the stamp
# matches SHERPA_VERSION AND every required .lib is present, we skip the
# download/stage path. To force re-stage: delete the stamp or run clean.ps1.
#
# NOTE (unlike sibling plugins): the staged .lib files are NOT committed
# to git -- onnxruntime.lib alone is ~685 MB, over GitHub's 100 MB hard
# limit. THIS SCRIPT is the restore path on a fresh clone:
#   git submodule update --init && ./setup-sherpa-onnx.ps1

$ErrorActionPreference = "Stop"

#---------------------------------------------------------------------
# 0. Paths
#---------------------------------------------------------------------
$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$SherpaOnnxDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir     = (Resolve-Path (Join-Path $SherpaOnnxDir "..")).Path
$VersionFile   = Join-Path $SherpaOnnxDir "SHERPA_VERSION"
$CacheDir      = Join-Path $SherpaOnnxDir ".cache"
$VendorSrcDir  = Join-Path $PluginDir "Vendor\sherpa-onnx"

$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty"
$Win64LibStageDir = Join-Path $ThirdPartyDir "Win64"

$StampFile = Join-Path $ThirdPartyDir ".sherpa_version"

# The complete static lib set from the win-x64-static-MD-Release-lib
# archive. All 14 are required -- TTS pulls espeak-ng/piper_phonemize/ucd,
# ASR pulls the kaldi/fst family, everything sits on the static
# onnxruntime. Linked in InoSherpa.Build.cs in this same order.
$RequiredLibs = @(
    "sherpa-onnx-c-api.lib",
    "sherpa-onnx-cxx-api.lib",
    "sherpa-onnx-core.lib",
    "sherpa-onnx-fst.lib",
    "sherpa-onnx-fstfar.lib",
    "sherpa-onnx-kaldifst-core.lib",
    "kaldi-decoder-core.lib",
    "kaldi-native-fbank-core.lib",
    "ssentencepiece_core.lib",
    "piper_phonemize.lib",
    "espeak-ng.lib",
    "ucd.lib",
    "kissfft-float.lib",
    "onnxruntime.lib"
)

#---------------------------------------------------------------------
# 1. Load pinned version
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "SHERPA_VERSION file not found at $VersionFile"
}
$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "SHERPA_VERSION must be a version like '1.13.3' (no leading v). Got: '$Version'"
}

Write-Host ""
Write-Host "=== sherpa-onnx setup ===" -ForegroundColor Cyan
Write-Host "sherpa-onnx version:  $Version (from SHERPA_VERSION)"
Write-Host "Plugin dir:           $PluginDir"
Write-Host "Vendor source:        $VendorSrcDir"
Write-Host "Cache dir:            $CacheDir"
Write-Host ""

#---------------------------------------------------------------------
# 2. Verify vendor submodule is checked out at the matching tag.
#    Headers come from the submodule at BUILD time, so this must hold
#    even when the libs are already staged -- check before idempotency.
#---------------------------------------------------------------------
if (-not (Test-Path (Join-Path $VendorSrcDir "CMakeLists.txt"))) {
    Write-Error @"
Vendor submodule not initialized at $VendorSrcDir.
Run from the InoSherpa plugin repo:
  git submodule update --init
"@
}

Write-Host "--- Verifying vendor submodule tag ---" -ForegroundColor Yellow
$VendorTag = ""
try {
    Push-Location $VendorSrcDir
    $VendorTag = (& git describe --tags --exact-match HEAD 2>$null)
    if ($null -ne $VendorTag) { $VendorTag = $VendorTag.Trim() }
} finally {
    Pop-Location
}
if ($VendorTag -ne "v$Version") {
    Write-Error @"
Vendor submodule is checked out at '$VendorTag', but SHERPA_VERSION is '$Version' (expects tag 'v$Version').
They MUST match -- the C API headers are consumed from the submodule and
must correspond to the staged release libs. To sync:
  git -C "$VendorSrcDir" fetch --tags
  git -C "$VendorSrcDir" checkout v$Version
"@
}
Write-Host "  Submodule tag: $VendorTag (matches SHERPA_VERSION)"

#---------------------------------------------------------------------
# 3. Idempotency: stamp + presence of every required staged lib
#---------------------------------------------------------------------
function Test-StagedComplete {
    if (-not (Test-Path $StampFile)) { return $false }
    if ((Get-Content $StampFile -Raw).Trim() -ne $Version) { return $false }
    foreach ($f in $RequiredLibs) {
        if (-not (Test-Path (Join-Path $Win64LibStageDir $f))) { return $false }
    }
    return $true
}

if (Test-StagedComplete) {
    Write-Host "--- Already up to date ---" -ForegroundColor Green
    Write-Host "  sherpa-onnx $Version staged (Win64 static, 14 libs)."
    Write-Host "  Delete '$StampFile' or bump SHERPA_VERSION to force re-stage."
    exit 0
}

#---------------------------------------------------------------------
# 4. Preflight: create directories + download helper
#---------------------------------------------------------------------
foreach ($d in @($CacheDir, $Win64LibStageDir)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

function Download-IfMissing {
    param([string]$Url, [string]$Dest, [string]$Label, [int]$MinSizeMb = 1)
    if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt ($MinSizeMb * 1MB))) {
        Write-Host "  [CACHED] $Label ($(Split-Path $Dest -Leaf))"
        return
    }
    Write-Host "  [DOWNLOAD] $Label"
    Write-Host "             $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    $sizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($sizeMb MB)"
}

#=====================================================================
# WIN64: download + stage upstream's prebuilt static-MD-Release libs
#=====================================================================
Write-Host "=== Win64 (prebuilt static) ===" -ForegroundColor Cyan

$ReleaseBase  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$Version"
$ArchiveName  = "sherpa-onnx-v$Version-win-x64-static-MD-Release-lib.tar.bz2"
$ArchivePath  = Join-Path $CacheDir $ArchiveName

Download-IfMissing -Url "$ReleaseBase/$ArchiveName" -Dest $ArchivePath `
    -Label "sherpa-onnx $Version Win64 static MD Release libs" -MinSizeMb 20

# Extract with Windows-native bsdtar (handles .tar.bz2; Expand-Archive
# cannot). Ships with Windows 10 1803+.
$TarExe = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $TarExe)) {
    Write-Error "tar.exe not found at $TarExe (requires Windows 10 1803+)."
}

$ExtractDir = Join-Path $CacheDir "win64-static-$Version"
if (Test-Path $ExtractDir) { Remove-Item -Recurse -Force $ExtractDir }
New-Item -ItemType Directory -Path $ExtractDir -Force | Out-Null

Write-Host "  [EXTRACT] $ArchiveName"
& $TarExe -xf $ArchivePath -C $ExtractDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "tar extraction failed (exit $LASTEXITCODE) for $ArchivePath"
}

# Locate the lib/ directory (archive layout: sherpa-onnx-v<ver>-...-lib/lib/*.lib)
$LibSrcDir = Get-ChildItem -Path $ExtractDir -Recurse -Directory |
    Where-Object { (Get-ChildItem -Path $_.FullName -Filter "*.lib" -File -ErrorAction SilentlyContinue).Count -gt 0 } |
    Select-Object -First 1
if ($null -eq $LibSrcDir) {
    Write-Error "No directory containing .lib files found under $ExtractDir -- upstream archive layout changed?"
}
Write-Host "  Lib source: $($LibSrcDir.FullName)"

# Wipe-then-stage so libs removed upstream don't linger.
Get-ChildItem -Path $Win64LibStageDir -Filter "*.lib" -File -ErrorAction SilentlyContinue | Remove-Item -Force
foreach ($f in $RequiredLibs) {
    $src = Join-Path $LibSrcDir.FullName $f
    if (-not (Test-Path $src)) {
        Write-Error "Required lib '$f' missing from upstream archive at $($LibSrcDir.FullName) -- layout drift?"
    }
    Copy-Item -Path $src -Destination (Join-Path $Win64LibStageDir $f) -Force
    $sizeMb = [math]::Round((Get-Item (Join-Path $Win64LibStageDir $f)).Length / 1MB, 1)
    Write-Host "  [STAGED] $f ($sizeMb MB)"
}

# Warn (don't fail) about unexpected extra libs upstream added -- a cue
# to revisit $RequiredLibs + InoSherpa.Build.cs on version bumps.
$ExtraLibs = Get-ChildItem -Path $LibSrcDir.FullName -Filter "*.lib" -File |
    Where-Object { $RequiredLibs -notcontains $_.Name }
foreach ($e in $ExtraLibs) {
    Write-Host "  [WARN] upstream archive contains unlisted lib: $($e.Name) (not staged -- add to RequiredLibs + Build.cs if needed)" -ForegroundColor Yellow
}

# Clean up the extraction scratch dir (keep the downloaded archive cached).
Remove-Item -Recurse -Force $ExtractDir

#---------------------------------------------------------------------
# 5. Write stamp
#---------------------------------------------------------------------
Set-Content -Path $StampFile -Value $Version -NoNewline -Encoding ASCII
Write-Host ""
Write-Host "--- Done ---" -ForegroundColor Green
Write-Host "  sherpa-onnx $Version staged: $($RequiredLibs.Count) static libs in Source/ThirdParty/Win64/"
