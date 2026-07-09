# get-dev-models.ps1
#
# Downloads the Phase-1 dev/smoke-test models into SherpaOnnx/models/
# (gitignored -- models are NEVER committed). These are the models the
# Ino.Sherpa.* console smoke tests are pointed at during development;
# shipping-model selection/packaging is the hosting project's concern.
#
#   TTS: vits-piper-en_US-libritts_r-medium
#     Piper VITS English voice (~75 MB). Bundle includes the model .onnx,
#     tokens.txt, and the espeak-ng-data/ dir Piper phonemization needs.
#
#   STT: sherpa-onnx-streaming-zipformer-en-2023-06-26
#     Streaming Zipformer transducer, English (~300 MB with fp32+int8
#     variants). Bundle includes encoder/decoder/joiner .onnx (+ -int8),
#     tokens.txt, and test_wavs/ for the smoke tests.
#
# Both are published on sherpa-onnx's GitHub model release tags
# (tts-models / asr-models) -- version-independent of the runtime pin.
# Idempotent: skips anything already extracted.

$ErrorActionPreference = "Stop"

$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$SherpaOnnxDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ModelsDir     = Join-Path $SherpaOnnxDir "models"
$CacheDir      = Join-Path $SherpaOnnxDir ".cache"

$Models = @(
    @{
        Name  = "vits-piper-en_US-libritts_r-medium"
        Url   = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-libritts_r-medium.tar.bz2"
        Label = "Piper VITS en_US libritts_r medium (TTS)"
    },
    @{
        Name  = "sherpa-onnx-streaming-zipformer-en-2023-06-26"
        Url   = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-en-2023-06-26.tar.bz2"
        Label = "Streaming Zipformer transducer en (STT, streaming)"
    },
    @{
        Name  = "sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8"
        Url   = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2"
        Label = "NVIDIA Parakeet-TDT 0.6B v2 int8 (STT, offline / high accuracy, ~460 MB)"
    }
)

$TarExe = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $TarExe)) {
    Write-Error "tar.exe not found at $TarExe (requires Windows 10 1803+)."
}

foreach ($d in @($ModelsDir, $CacheDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

foreach ($m in $Models) {
    $destDir = Join-Path $ModelsDir $m.Name
    if (Test-Path $destDir) {
        Write-Host "[PRESENT] $($m.Name)" -ForegroundColor Green
        continue
    }
    $archive = Join-Path $CacheDir "$($m.Name).tar.bz2"
    if (-not ((Test-Path $archive) -and ((Get-Item $archive).Length -gt 1MB))) {
        Write-Host "[DOWNLOAD] $($m.Label)"
        Write-Host "           $($m.Url)"
        Invoke-WebRequest -Uri $m.Url -OutFile $archive -UseBasicParsing
    } else {
        Write-Host "[CACHED] $($m.Name).tar.bz2"
    }
    Write-Host "[EXTRACT] $($m.Name)"
    & $TarExe -xf $archive -C $ModelsDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "tar extraction failed (exit $LASTEXITCODE) for $archive"
    }
    if (-not (Test-Path $destDir)) {
        Write-Error "Extraction did not produce $destDir -- archive layout changed?"
    }
    Write-Host "[READY] $destDir" -ForegroundColor Green
}

Write-Host ""
Write-Host "Dev models ready under $ModelsDir" -ForegroundColor Green
