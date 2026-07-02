# clean.ps1
#
# Wipes everything setup-sherpa-onnx.ps1 produced:
#   - SherpaOnnx/.cache/            (downloaded archives + extraction scratch)
#   - Source/ThirdParty/Win64/*.lib (staged static libs)
#   - Source/ThirdParty/.sherpa_version (idempotency stamp)
#
# Does NOT touch Vendor/sherpa-onnx (that's a git submodule / source) or
# SherpaOnnx/models/ (dev models -- wipe those by hand if wanted).
# Re-run setup-sherpa-onnx.ps1 to restore.

$ErrorActionPreference = "Stop"

$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$SherpaOnnxDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir     = (Resolve-Path (Join-Path $SherpaOnnxDir "..")).Path

$CacheDir         = Join-Path $SherpaOnnxDir ".cache"
$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty"
$Win64LibStageDir = Join-Path $ThirdPartyDir "Win64"
$StampFile        = Join-Path $ThirdPartyDir ".sherpa_version"

if (Test-Path $CacheDir) {
    Write-Host "Removing $CacheDir"
    Remove-Item -Recurse -Force $CacheDir
}
if (Test-Path $Win64LibStageDir) {
    Write-Host "Removing staged libs in $Win64LibStageDir"
    Get-ChildItem -Path $Win64LibStageDir -Filter "*.lib" -File -ErrorAction SilentlyContinue | Remove-Item -Force
}
if (Test-Path $StampFile) {
    Write-Host "Removing stamp $StampFile"
    Remove-Item -Force $StampFile
}
Write-Host "Clean complete. Re-run setup-sherpa-onnx.ps1 to restore." -ForegroundColor Green
