# CLAUDE.md — InoSherpa plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoSherpa/`. The hosting demo project is documented in
the project root's `CLAUDE.md`.

## Purpose

`InoSherpa` is an Unreal Engine 5.8 runtime plugin that **integrates
k2-fsa's sherpa-onnx speech toolkit statically and exposes two
Blueprint-callable game-instance subsystems**:

- **`UInoTTS`** — on-device text-to-speech (Piper/VITS shipping; Kokoro
  config plumbed, wiring next). Sync, async, and streaming (per-sentence
  audio chunks) APIs, mid-generation cancellation.
- **`UInoSTT`** — on-device speech-to-text. TWO independent models can
  be loaded side by side:
  - *Streaming* (Zipformer transducer): push-only audio input, live
    partial results, endpoint auto-segmentation.
  - *Offline* (NeMo transducer / **Parakeet-TDT** tested; Whisper,
    SenseVoice, Moonshine configs plumbed): whole-clip
    `TranscribeOffline*` one-shots — noticeably higher accuracy with
    punctuation + casing, RTF ~0.04 on CPU. Offline one-shots may run
    while a streaming session is live (separate inference sessions) but
    serialize among themselves.

Upstream: https://github.com/k2-fsa/sherpa-onnx — it also offers VAD,
speaker ID/diarization, keyword spotting, etc., which can slot into this
plugin later using the same patterns.

## Status / roadmap

1. ✅ **Phase 1 — Win64** (shipped): official static release libs staged
   by setup script; both subsystems implemented and smoke-tested against
   Piper (`vits-piper-en_US-libritts_r-medium`) and streaming Zipformer
   (`sherpa-onnx-streaming-zipformer-en-2023-06-26`).
2. ⏳ **Phase 2 — Android** (arm64-v8a): self-built static libs from
   `Vendor/sherpa-onnx` (see "Why Android is self-built").
3. ⏳ **Phase 3 — iOS / macOS**: official static release artifacts.

## The one core decision: static everywhere

sherpa-onnx bundles its **own** ONNX Runtime (their build, CPU-only).
The engine process is already crowded with ONNX Runtimes: UE's NNE
plugin ships `onnxruntime.dll` (1.19.x), marketplace plugins ship their
own, and our sibling `InoOnnx` stages a renamed DirectML build — see
`Plugins/InoOnnx/CLAUDE.md` "Why we rename the DLLs / .so" for the full
failure-mode catalog (Windows base-name DLL cache, Android SONAME
aliasing).

sherpa's **shared** release packages ship a plain-named
`onnxruntime.dll` / `libonnxruntime.so` — which would re-create exactly
those collisions.

**Therefore InoSherpa links sherpa-onnx and its ONNX Runtime STATICALLY
on every platform.** Everything ends up inside `UnrealEditor-InoSherpa.dll`
(or the platform equivalent) with no exports; no plain-named ORT binary
ever ships. Collisions are structurally impossible with zero rename
surgery.

Consequences:

- InoSherpa's ORT is CPU-only (all upstream's static packages contain).
  Fine for speech workloads; `InoOnnx` remains the accelerated
  (DirectML) path for everything else. The two ORT copies coexist.
- `LoadingPhase=Default` (NOT `PreLoadingScreen` like the DLL-staging
  siblings) — there is no runtime binary to pre-load.
- No Live Coding DLL-notification hazards (no runtime DLLs at all) —
  contrast with InoLlama's `%TEMP%` scratch-dir workaround.

## UE-side architecture

One module, two subsystems, three module-private native classes:

| Class | Kind | Role |
|---|---|---|
| `UInoTTS` (`Public/TTS/InoTTSSubsystem.h`) | `UGameInstanceSubsystem` | TTS API surface; Pattern-A threading |
| `UInoSTT` (`Public/STT/InoSTTSubsystem.h`) | `UGameInstanceSubsystem` | STT API surface; owns the stream worker |
| `FInoTtsEngine` (`Private/TTS/`) | plain C++, `TSharedPtr` | owns `SherpaOnnxOfflineTts`; blocking `Generate` with cancel/chunk trampoline |
| `FInoSttRecognizer` (`Private/STT/`) | plain C++, `TSharedPtr` | owns `SherpaOnnxOnlineRecognizer`; `TranscribeOnce` for streaming-model one-shots |
| `FInoSttOfflineRecognizer` (`Private/STT/`) | plain C++, `TSharedPtr` | owns `SherpaOnnxOfflineRecognizer` (Parakeet et al); `Transcribe` whole clips — no tail padding needed (full-context decode) |
| `FInoSttStreamWorker` (`Private/STT/`) | `FRunnable` | exclusively owns one `SherpaOnnxOnlineStream` + its thread |

Threading (copied from the InoAgents house patterns — see
`Plugins/InoAgents/Source/InoNeuTTS/.../InoNeuTTSSubsystem.cpp` and
`.../InoLiteRtLmConversationWorker.h`):

- **All public subsystem methods are game-thread-only; all delegates
  fire on the game thread.**
- **TTS = Pattern A**: `Async(ThreadPool)` per operation +
  `AsyncTask(GameThread)` marshaling + `TWeakObjectPtr` guards + per-op
  `TSharedPtr<std::atomic<bool>>` cancel flag + `bSynthInFlight`
  single-op invariant. Cancellation is real: the sherpa progress
  callback returns 0 and generation stops; the result carries partial
  audio + `bWasCancelled`.
- **STT = Pattern B**: sherpa's online API has NO internal locking —
  every call on a stream must be serialized. `FInoSttStreamWorker` is
  the only thread touching the stream: game thread produces into an
  SPSC `TQueue` (+ `FEvent` wake), worker consumes
  (AcceptWaveform → decode pump → dispatch). Its destructor **joins the
  thread before destroying the stream** (never free mid-call).
- One-shot `Transcribe*` is refused while a streaming session is active
  (single-decode-at-a-time invariant — concurrent decoding of two
  streams of one recognizer has no upstream thread-safety guarantee).

STT streaming behavior:

- Partials dispatch only when the text changed (anti-flood).
- Endpoint (pause detected) → `OnFinal` + `OnEndpoint` + auto stream
  reset; silence-only endpoints reset quietly (no event spam).
- `FinishStream` appends **0.6 s of silence tail-padding** before
  `InputFinished` — without it the utterance's last word is truncated
  (upstream examples pad the same way) — then recreates the stream so
  the session survives for the next utterance.

## Settings-driven model downloads

`UInoSherpaSettings` (Project Settings → Plugins → InoSherpa,
`Public/InoSherpaSettings.h`, Config=Game defaultconfig) holds one
download source per STT model — `StreamingSttModel` (Zipformer) and
`OfflineSttModel` (Parakeet). sherpa models are multi-file, so each
source is FOUR file slots (Encoder/Decoder/Joiner/Tokens), each with
`Url`, optional `LocalFileName` (empty = derived from URL), optional
`ExpectedSha256`, optional `FileSizeBytes`. Defaults point at the public
Hugging Face mirrors (int8 variants; ~70 MB Zipformer, ~630 MB
Parakeet) — override with your own CDN for shipping.

`UInoSTT::LoadStreamingModelFromSettingsAsync` /
`LoadOfflineModelFromSettingsAsync` download whatever is missing via the
**InoNodes** downloader (cached-skip, resume, retries, optional SHA-256,
`OnDownloadProgress` per tick with batch-wide `OverallProgressPercent`)
into `<ProjectPersistentDownloadDir>/InoSherpa/<ModelDirName>/`, then
chain into the existing load path. `IsStreamingModelDownloaded()` /
`IsOfflineModelDownloaded()` are stat-probes (UMG-safe);
`CancelModelDownload()` aborts mid-download; `Deinitialize` cancels
automatically. This mirrors the InoAgents Gemma flow
(`UInoLiteRtLmSettings` + `LoadModelAsync`) adapted for multi-file
models.

InoSherpa therefore depends on the **InoNodes** plugin (`.uplugin`
Plugins list + `PublicDependencyModuleNames`) — a base-utility
dependency, same direction as InoAgents' use of it.

TTS (Piper) is NOT settings-driven yet: Piper bundles include the
espeak-ng-data DIRECTORY (hundreds of files) which doesn't fit per-file
downloads — needs an archive step; revisit when TTS model distribution
matters.

## Wire format

`TArray<uint8>` **int16 mono PCM little-endian** + `SampleRate` metadata
(the cross-plugin lingua franca). TTS emits the model's native rate
(22050 for Piper medium) and never resamples — playback is the caller's
concern (RuntimeAudioImporter's `UStreamingSoundWave` /
`USoundWaveProcedural`; UE's mixer converts rates at the sink). STT
accepts pushes at ANY rate (int16 bytes or float32) — sherpa resamples
internally to the model's 16 kHz.

`Private/InoSherpaPcm.{h,cpp}` holds the float32↔int16 converters + a
strict mono WAV reader/writer (mirrors `UInoAudioFunctionLibrary` shapes
WITHOUT depending on InoAgents — runtime plugins never depend on their
consumers).

## Setup

```powershell
cd Plugins/InoSherpa/SherpaOnnx/scripts
./setup-sherpa-onnx.ps1     # stage the Win64 static libs (idempotent)
./get-dev-models.ps1        # optional: Piper + Zipformer dev models
```

The setup script reads `SherpaOnnx/SHERPA_VERSION` (currently `1.13.3`),
verifies `Vendor/sherpa-onnx` is checked out at tag `v<ver>` (headers
are consumed from the submodule — they MUST match the staged libs),
downloads `sherpa-onnx-v<ver>-win-x64-static-MD-Release-lib.tar.bz2`
from GitHub releases into gitignored `SherpaOnnx/.cache/`, and stages
all 14 static libs into `Source/ThirdParty/Win64/`. Idempotency stamp:
`Source/ThirdParty/.sherpa_version`. `clean.ps1` wipes it all.

**Staged libs are NOT committed** (unlike sibling plugins'
`Source/ThirdParty` outputs): `onnxruntime.lib` alone is ~685 MB — over
GitHub's 100 MB hard limit. Fresh clones run:

```powershell
git submodule update --init
./SherpaOnnx/scripts/setup-sherpa-onnx.ps1
```

`InoSherpa.Build.cs` throws a `BuildException` pointing at the script
when the libs are missing.

### Bumping the pin

```powershell
# 1. Edit SherpaOnnx/SHERPA_VERSION (e.g. 1.14.0)
# 2. Sync vendor submodule to the SAME tag (the script enforces this)
git -C Plugins/InoSherpa/Vendor/sherpa-onnx fetch --tags
git -C Plugins/InoSherpa/Vendor/sherpa-onnx checkout v1.14.0
# 3. Re-run setup; it warns about new unlisted libs in the archive
./setup-sherpa-onnx.ps1
```

Upstream has **no release branch** — master + version tags; prebuilt
assets exist only for tags.

## Build wiring (Win64)

- The 14 `.lib` files go through `PublicAdditionalLibraries`; no
  `RuntimeDependencies` / delay-load (nothing to stage — that's the
  point of static).
- `PrivateIncludePaths` → `Vendor/sherpa-onnx` for
  `#include "sherpa-onnx/c-api/c-api.h"`. **PRIVATE on purpose: sherpa
  types never leak past this module.** Consumers only see `Ino*` types.
- **Define nothing**: with neither `SHERPA_ONNX_BUILD_SHARED_LIBS` nor
  `SHERPA_ONNX_BUILD_MAIN_LIB` defined, `SHERPA_ONNX_API` expands empty
  on Win32 — plain declarations, correct for static linking.
- MD-Release libs match UE's `/MD` CRT. No extra Win32 system libs were
  needed. `bUseUnity=false` (file-static console commands in smoke-test
  TUs).
- Use the non-deprecated `SherpaOnnxOfflineTtsGenerateWithConfig` (the
  older `SherpaOnnxOfflineTtsGenerate*` family is `SHERPA_ONNX_DEPRECATED`
  → C4996).

## Smoke tests

Console commands (self-registering `FAutoConsoleCommand`s under
`Private/SmokeTests/`; all need a PIE / `-game` world; delegate landing
pads are small `UObject` helpers since dynamic delegates bind UFUNCTIONs
only):

| Command | What it proves |
|---|---|
| `Ino.Sherpa.TTS.LoadTest <vits.onnx> <tokens.txt> <espeak-ng-data-dir>` | sync Piper load; logs rate/speakers |
| `Ino.Sherpa.TTS.SynthTest <text...>` | streaming synth; chunk delegates; writes `Saved/InoSherpa/tts.wav` |
| `Ino.Sherpa.TTS.CancelTest [text...]` | mid-generation cancel → partial audio + `bWasCancelled` |
| `Ino.Sherpa.STT.LoadTest <enc> <dec> <joiner> <tokens>` | sync Zipformer load |
| `Ino.Sherpa.STT.TranscribeTest <mono.wav>` | sync one-shot; text matches `test_wavs/trans.txt` |
| `Ino.Sherpa.STT.StreamTest <mono.wav> [passes]` | full worker path: paced 100 ms pushes → growing partials → final; passes ≥ 2 exercises session reuse after FinishStream |
| `Ino.Sherpa.STT.AbortTest <mono.wav>` | StopStream joining mid-decode, then a fresh session |
| `Ino.Sherpa.STT.OfflineLoadTest <enc> <dec> <joiner> <tokens>` | sync Parakeet (NeMo transducer) load |
| `Ino.Sherpa.STT.OfflineTranscribeTest <mono.wav>` | sync offline one-shot (punctuated, cased text) |
| `Ino.Sherpa.STT.OfflineTranscribeAsyncTest <mono.wav>` | async offline one-shot via delegate |
| `Ino.Sherpa.STT.SettingsLoadTest [streaming\|offline]` | Project-Settings download (cached-skip) → load, with progress ticks |

Headless one-liner used for verification (from a shell):

```
UnrealEditor-Cmd.exe <uproject> -game -NullRHI -unattended -nosplash -nosound
    -ExecCmds="Ino.Sherpa.STT.LoadTest ..., Ino.Sherpa.STT.StreamTest ..." -abslog=<log>
```

## Layout

```
Plugins/InoSherpa/
├── CLAUDE.md                        ← this file
├── InoSherpa.uplugin                ← Runtime module, LoadingPhase=Default
├── Vendor/
│   └── sherpa-onnx/                 ← upstream git submodule @ v1.13.3
│                                      (headers for all platforms; source
│                                       for the phase-2 Android self-build)
├── SherpaOnnx/                      ← setup workspace
│   ├── SHERPA_VERSION               ← pinned upstream version ("1.13.3")
│   ├── scripts/
│   │   ├── setup-sherpa-onnx.ps1    ← stages Win64 static libs, enforces
│   │   │                              submodule tag match
│   │   ├── clean.ps1                ← wipes cache + staged libs + stamp
│   │   └── get-dev-models.ps1       ← Piper + Zipformer dev models
│   ├── .cache/                      ← downloads (gitignored)
│   └── models/                      ← dev models (gitignored)
└── Source/
    ├── ThirdParty/
    │   ├── .sherpa_version          ← stamp (gitignored)
    │   └── Win64/                   ← 14 staged .lib (GITIGNORED, ~771 MB;
    │                                  onnxruntime.lib alone 685 MB — setup
    │                                  script is the restore path)
    └── InoSherpa/
        ├── InoSherpa.Build.cs
        ├── Public/
        │   ├── InoSherpa.h          ← module + LogInoSherpa
        │   ├── Sherpa/InoSherpaTypes.h  ← EInoSherpaProvider, EInoTTSModelType
        │   ├── TTS/InoTTSTypes.h    ← configs/options/result + delegates
        │   ├── TTS/InoTTSSubsystem.h← UInoTTS
        │   ├── STT/InoSTTTypes.h    ← config/result + delegates
        │   └── STT/InoSTTSubsystem.h← UInoSTT
        └── Private/
            ├── InoSherpa.cpp        ← logs sherpa version (link sanity)
            ├── InoSherpaPcm.{h,cpp} ← int16<->float32 + WAV read/write
            ├── TTS/InoTtsEngine.{h,cpp}
            ├── TTS/InoTTSSubsystem.cpp
            ├── STT/InoSttRecognizer.{h,cpp}
            ├── STT/InoSttStreamWorker.{h,cpp}
            ├── STT/InoSTTSubsystem.cpp
            └── SmokeTests/          ← console commands + UObject helpers
```

## How other plugins consume this

```csharp
PublicDependencyModuleNames.Add("InoSherpa");
```

```json
"Plugins": [ { "Name": "InoSherpa", "Enabled": true } ]
```

```cpp
#include "TTS/InoTTSSubsystem.h"
UInoTTS* Tts = GetGameInstance()->GetSubsystem<UInoTTS>();
Tts->LoadModelAsync(Config, OnLoaded);
Tts->SynthesizeStreamAsync(Text, Options, OnChunk, OnComplete);
```

Blueprint reaches both subsystems via the standard Get Game Instance
Subsystem node. Delegates carry `FInoTTSResult` / `FInoSTTResult`.

## Per-platform artifact strategy (phases 2-3 reference)

| Platform | Source of static libs | Notes |
|---|---|---|
| **Win64** ✅ | Official `win-x64-static-MD-Release-lib` release package | shipped |
| **Android** arm64-v8a | **Self-built** from `Vendor/sherpa-onnx` | No usable static release exists: the `android-static-link-onnxruntime` artifact is **JNI-only** (no `libsherpa-onnx-c-api.so`) and the regular android tarball ships a plain-named `libonnxruntime.so` (SONAME hazard). Build with `SHERPA_ONNX_ENABLE_C_API=ON`, `SHERPA_ONNX_ENABLE_JNI=OFF`, static ORT; upstream's `build-android-arm64-v8a.sh` supports exactly this (needs an NDK). |
| **iOS** | Official `ios.tar.bz2` | `sherpa-onnx.xcframework` (merged `libsherpa-onnx.a` + C API headers) + separate static `onnxruntime.xcframework`. ⚠ InoOnnx on iOS also statically links its ORT into the executable — shipping both requires unifying on ONE ORT static archive (duplicate `Ort*` symbols otherwise). |
| **macOS** | Official `osx-{arm64,universal2}-static` packages | same shape as Win64 |

sherpa's ORT builds come from https://github.com/csukuangfj/onnxruntime-libs;
the version is dictated by the sherpa release, not by us, and does NOT
need to match InoOnnx's pin — the copies are isolated.

## Naming

All plugin-facing types are `Ino`-prefixed (`UInoTTS`, `UInoSTT`,
`FInoTTS*`, `FInoSTT*`, `EInoSherpa*`), matching the rule enforced
across the sibling plugins. Upstream sherpa symbols stay behind the
module boundary.

## Git hygiene

This directory is its **own git repository** (vendored-but-live inside
the demo project). Commits go to this repo with
`git -C Plugins/InoSherpa …`, never through the project repo.
`Vendor/sherpa-onnx` is a submodule of THIS repo; fresh clones need
`git submodule update --init` + the setup script (staged libs are not
in git — see "Setup").

## Authoritative references

- sherpa-onnx repo: https://github.com/k2-fsa/sherpa-onnx
- Releases (prebuilt artifacts + models): https://github.com/k2-fsa/sherpa-onnx/releases
- Docs: https://k2-fsa.github.io/sherpa/onnx/
- C API header (in submodule): `Vendor/sherpa-onnx/sherpa-onnx/c-api/c-api.h`
- Upstream Android build script: `Vendor/sherpa-onnx/build-android-arm64-v8a.sh`
- Sibling plugin docs this plugin leans on:
  - `Plugins/InoOnnx/CLAUDE.md` — the ORT-collision failure catalog the
    static-everywhere decision avoids
  - `Plugins/InoLlama/CLAUDE.md` — the setup-script / version-pin /
    vendor-submodule-sync pattern this plugin copies
  - `Plugins/InoAgents` (NeuTTS subsystem + LiteRtLm worker) — the
    threading + delegate house patterns the subsystems copy
