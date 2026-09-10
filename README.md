# InoSherpa

**On-device speech for Unreal Engine 5 — text-to-speech and speech-to-text, fully offline, no cloud, no API keys.**

InoSherpa wraps [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
as a UE5 runtime plugin and exposes it as two Blueprint-callable game
instance subsystems:

| Subsystem | What it does |
|---|---|
| **`UInoTTS`** | Text-to-speech (Piper / VITS). Sync, async, and streaming (per-sentence audio chunks) with real mid-generation cancellation. |
| **`UInoSTT`** | Speech-to-text. Two independent models side by side: a **streaming** Zipformer (live partial results, automatic pause detection) and an **offline** Parakeet-TDT (whole-clip one-shots, punctuated and cased, ~0.04 RTF on CPU). |

Everything runs locally on the CPU. No network calls at runtime beyond the
one-time model download you control.

---

## Status

| Platform | State |
|---|---|
| **Win64** | ✅ Shipping — official static release libs, both subsystems smoke-tested |
| **Android** (arm64-v8a) | ⏳ Planned — needs a self-built static lib set |
| **iOS / macOS** | ⏳ Planned — official static release artifacts |

On every non-Win64 platform the module still **compiles and links** as a
graceful stub: `UInoTTS` / `UInoSTT` exist so cooked Blueprint references
resolve, and model loads fail with a clear error instead of breaking the
build. You can put InoSherpa in a cross-platform project today; only the
Windows target will actually synthesize and transcribe.

---

## Requirements

- **Unreal Engine 5.8**
- **Windows 10 1803+** for the setup script (`tar.exe`), Visual Studio toolchain for building
- **[InoNodes](https://github.com/nobandegani/ino-nodes-ue)** — companion Inoland plugin providing the resumable file downloader that model auto-download is built on. Install it alongside InoSherpa in your `Plugins/` directory.
- ~1 GB of disk for the staged static libs, plus model storage

---

## Install

Clone into your project's `Plugins/` directory as `InoSherpa`, then restore
the static libraries:

```bash
git clone --recurse-submodules https://github.com/nobandegani/ino-sherpa-ue.git Plugins/InoSherpa
```

```powershell
cd Plugins/InoSherpa/SherpaOnnx/scripts
./setup-sherpa-onnx.ps1
```

If you cloned without `--recurse-submodules`, run `git submodule update --init`
first — the sherpa-onnx C API headers are consumed straight from
`Vendor/sherpa-onnx`, and the setup script enforces that the submodule sits
on the exact tag matching `SherpaOnnx/SHERPA_VERSION`.

### Why a setup script instead of committed libs

The staged static libraries total roughly 771 MB — `onnxruntime.lib` alone
is ~685 MB, far past GitHub's 100 MB hard limit. `setup-sherpa-onnx.ps1` is
the deterministic restore path: it downloads the pinned official release
archive into a gitignored cache and stages all 14 `.lib` files into
`Source/ThirdParty/Win64/`. It's idempotent (stamped by
`Source/ThirdParty/.sherpa_version`), so re-running it is a cheap no-op.
`clean.ps1` wipes everything it produced.

`InoSherpa.Build.cs` throws a build error pointing at the script if the libs
are missing, so you can't accidentally build against a half-set-up tree.

### Optional: dev models for the smoke tests

```powershell
./get-dev-models.ps1
```

Pulls a Piper voice, the streaming Zipformer, and Parakeet-TDT into the
gitignored `SherpaOnnx/models/`.

---

## Quick start

Both subsystems are reachable from Blueprint via the standard **Get Game
Instance Subsystem** node.

### Speech-to-text

Model sources live in **Project Settings → Plugins → InoSherpa**. The
defaults point at public Hugging Face mirrors of the sherpa-onnx reference
models, so loading works out of the box:

```cpp
#include "STT/InoSTTSubsystem.h"

UInoSTT* Stt = GetGameInstance()->GetSubsystem<UInoSTT>();

// Downloads whatever is missing (resume, retries, cached-skip), then loads.
Stt->LoadStreamingModelAsync(Options, OnDownloadProgress, OnLoaded);

// Then push audio from any source, at any sample rate:
Stt->StartStream(OnPartial, OnFinal, OnEndpoint);
Stt->PushAudioInt16(PcmBytes, 48000);   // partials arrive as you talk
Stt->FinishStream();                     // flush the tail -> OnFinal
```

`LoadStreamingModelAsync` and `LoadOfflineModelAsync` are the only Blueprint
loaders. Runtime knobs (thread count, decoding method, endpoint rules) are
**pins on the load node** with sensible defaults — file paths never appear
in Blueprint. `IsStreamingModelDownloaded()` / `IsOfflineModelDownloaded()`
are cheap stat probes, safe to poll from UMG, and `CancelModelDownload()`
aborts a download in progress.

For the higher-accuracy path, load the offline model and hand it whole
clips:

```cpp
Stt->LoadOfflineModelAsync(Options, OnDownloadProgress, OnLoaded);
FInoSTTResult Result = Stt->TranscribeOfflineInt16(PcmBytes, 16000);
// -> "Hello there, how are you doing today?"  (punctuated, cased)
```

The two models are independent inference sessions: an offline one-shot may
run while a streaming session is live.

### Text-to-speech

```cpp
#include "TTS/InoTTSSubsystem.h"

UInoTTS* Tts = GetGameInstance()->GetSubsystem<UInoTTS>();
Tts->LoadModelAsync(Config, OnLoaded);

// Streaming: OnChunk fires per generated sentence, then once more empty
// with bIsFinal = true; OnComplete carries the full concatenated clip.
Tts->SynthesizeStreamAsync(Text, Options, OnChunk, OnComplete);

Tts->CancelSynthesis();  // real cancellation -- partial audio + bWasCancelled
```

TTS is not settings-driven yet: Piper bundles ship an `espeak-ng-data`
directory of hundreds of small files, which doesn't fit the per-file
download model. Point `FInoTTSModelConfig` at a model on disk for now.

---

## Audio format

`TArray<uint8>` of **int16 mono PCM, little-endian**, plus a `SampleRate`
field — the wire format shared across the Ino plugin family.

- **TTS output** is at the model's native rate (22050 Hz for Piper medium)
  and is never resampled. Playback is the caller's concern: feed
  RuntimeAudioImporter's `UStreamingSoundWave` or a `USoundWaveProcedural`
  and let UE's mixer convert at the sink.
- **STT input** accepts any sample rate, as int16 bytes or float32 — sherpa
  resamples internally to the model's 16 kHz. Keep the rate *consistent*
  within a streaming session; sherpa locks a stream's resampler to the first
  rate it sees, and mismatched chunks are dropped with an error.

---

## Threading contract

**All public subsystem methods are game-thread-only, and all delegates fire
on the game thread.** You never need to marshal anything yourself.

Internally:

- **TTS** runs each operation on the engine thread pool with a per-operation
  atomic cancel flag, weak-object guards on every callback, and a
  single-synthesis-in-flight invariant.
- **STT** uses a dedicated worker thread that exclusively owns the sherpa
  stream — sherpa's online API has no internal locking, so every call on a
  stream must be serialized. The game thread produces into a lock-free SPSC
  queue; the worker consumes. Its destructor joins the thread *before*
  destroying the stream, so a stream is never freed mid-call.
- Partial results dispatch only when the text actually changed, so UI isn't
  flooded. A detected pause (endpoint) emits a final result plus an endpoint
  cue and auto-resets for the next utterance; silence-only endpoints reset
  quietly.

---

## Architecture: static everywhere

**This is the one design decision the whole plugin is built around.**

sherpa-onnx bundles its own ONNX Runtime build. The UE process is already
crowded with them: the built-in NNE plugin ships `onnxruntime.dll`,
marketplace plugins ship their own, and sibling Inoland plugins stage a
DirectML build. sherpa's *shared* release packages ship a plain-named
`onnxruntime.dll` / `libonnxruntime.so`, which would collide with all of
them — Windows resolves DLLs by base name, and Android aliases by SONAME.

So InoSherpa links sherpa-onnx **and its ONNX Runtime statically on every
platform.** Everything ends up inside the module binary with no exports; no
plain-named ORT binary ever ships. Collisions become structurally impossible
with zero renaming surgery.

Consequences worth knowing:

- InoSherpa's ONNX Runtime is **CPU-only** — that's all upstream's static
  packages contain. Fine for speech workloads.
- `LoadingPhase` is `Default`, not `PreLoadingScreen`: there is no runtime
  binary to pre-load.
- No Live Coding DLL-notification hazards, because there are no runtime DLLs
  at all.
- sherpa types never leak past the module boundary — the include path is
  private, and consumers only ever see `Ino*` types.

---

## Smoke tests

Console commands, all requiring a running PIE or `-game` world:

| Command | Proves |
|---|---|
| `Ino.Sherpa.TTS.LoadTest <model.onnx> <tokens.txt> <espeak-ng-data>` | Piper load; logs rate and speaker count |
| `Ino.Sherpa.TTS.SynthTest <text...>` | Streaming synthesis; writes `Saved/InoSherpa/tts.wav` |
| `Ino.Sherpa.TTS.CancelTest [text...]` | Mid-generation cancel → partial audio + `bWasCancelled` |
| `Ino.Sherpa.STT.LoadTest <enc> <dec> <joiner> <tokens>` | Streaming Zipformer load |
| `Ino.Sherpa.STT.TranscribeTest <mono.wav>` | Sync one-shot on the streaming model |
| `Ino.Sherpa.STT.StreamTest <mono.wav> [passes]` | Full worker path: paced pushes → growing partials → final. `passes >= 2` exercises session reuse |
| `Ino.Sherpa.STT.AbortTest <mono.wav>` | Tearing down mid-decode, then a clean fresh session |
| `Ino.Sherpa.STT.OfflineLoadTest <enc> <dec> <joiner> <tokens>` | Parakeet (NeMo transducer) load |
| `Ino.Sherpa.STT.OfflineTranscribeTest <mono.wav>` | Offline one-shot — punctuated, cased |
| `Ino.Sherpa.STT.OfflineTranscribeAsyncTest <mono.wav>` | Async offline one-shot via delegate |
| `Ino.Sherpa.STT.SettingsLoadTest [streaming\|offline]` | Project Settings download → load, with progress ticks |

Headless run:

```bash
UnrealEditor-Cmd.exe <uproject> -game -NullRHI -unattended -nosplash -nosound -ExecCmds="Ino.Sherpa.STT.LoadTest ...,Ino.Sherpa.STT.StreamTest ..." -abslog=<log>
```

---

## Updating the sherpa-onnx pin

The pinned version lives in `SherpaOnnx/SHERPA_VERSION` (currently
**1.13.3**) and must match the `Vendor/sherpa-onnx` submodule tag — headers
come from the submodule, libs from the release archive, and they have to
agree.

```powershell
# 1. Edit SherpaOnnx/SHERPA_VERSION
git -C Vendor/sherpa-onnx fetch --tags
git -C Vendor/sherpa-onnx checkout v<new-version>
./SherpaOnnx/scripts/setup-sherpa-onnx.ps1   # warns about new unlisted libs
```

Upstream has no release branch — master plus version tags, and prebuilt
assets exist only for tags.

---

## License

InoSherpa's own source is licensed under the **Mozilla Public License 2.0**
— see [LICENSE](LICENSE). Every source file carries the standard MPL
Exhibit A notice.

> Those notices deliberately **omit** the "Incompatible With Secondary
> Licenses" marking. That omission is load-bearing: MPL-2.0 §3.3 only permits
> combining MPL code with GPL code when the files are *not* so marked, and the
> TTS path below links GPL-3.0 libraries. Adding that marking would make the
> TTS build unlawful to distribute.

### Third-party components

The plugin statically links the following into its module binary:

| Component | License | Needed by |
|---|---|---|
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) | Apache-2.0 | everything |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | MIT | everything |
| [kaldi-decoder](https://github.com/k2-fsa/kaldi-decoder), [kaldi-native-fbank](https://github.com/csukuangfj/kaldi-native-fbank), [OpenFST](https://github.com/csukuangfj/openfst) | Apache-2.0 | STT |
| [simple-sentencepiece](https://github.com/pkufool/simple-sentencepiece) | Apache-2.0 | STT |
| [kissfft](https://github.com/mborgerding/kissfft) | BSD-3-Clause | everything |
| **[espeak-ng](https://github.com/espeak-ng/espeak-ng)** | **GPL-3.0** | TTS |
| **[piper-phonemize](https://github.com/csukuangfj/piper-phonemize)** | **GPL-3.0** | TTS |
| `ucd.lib` — built from espeak-ng's bundled `ucd-tools` | MIT | TTS |

> ⚠️ **The text-to-speech path is GPL-3.0.** Piper/VITS phonemization
> depends on `piper_phonemize.lib` and `espeak-ng.lib`, both GPL-3.0, and
> both are statically linked into the module binary. This does not affect
> the license of the source in this repository, but **any binary you
> distribute** that includes this module — a packaged game, a precompiled
> plugin — is a combined work subject to GPL-3.0 terms. Review this against
> your distribution plans before shipping.
>
> **Speech-to-text itself is unaffected** — every library the STT path needs
> is permissively licensed (Apache-2.0 / MIT / BSD-3-Clause). Building an
> STT-only, GPL-free binary means dropping `espeak-ng.lib`,
> `piper_phonemize.lib`, and `ucd.lib` from the link list in
> `InoSherpa.Build.cs` *and* compiling out `UInoTTS` — the TTS subsystem
> calls `SherpaOnnxCreateOfflineTts`, which is what drags those objects into
> the link in the first place. Just removing the libraries will fail to
> link.

### Speech models

Models are **not** in this repository — `SherpaOnnx/scripts/get-dev-models.ps1`
downloads them from sherpa-onnx's release assets. They are licensed separately
by their publishers, and the defaults are not uniform:

| Model | Used by | Origin |
|---|---|---|
| `vits-piper-en_US-libritts_r-medium` | TTS | Piper / VITS, trained on LibriTTS-R |
| `sherpa-onnx-streaming-zipformer-en-2023-06-26` | STT (streaming) | k2-fsa |
| `sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8` | STT (offline) | **NVIDIA NeMo** |

> **Check each model's license before shipping it** — particularly the NVIDIA
> Parakeet one, since NeMo model releases carry their own terms that are not
> the same as sherpa-onnx's Apache-2.0 code license, and LibriTTS-R-derived
> voices carry attribution conditions. A model's license is independent of the
> runtime's, and of this plugin's.

---

## Related

Part of the Inoland plugin family. Upstream sherpa-onnx also offers voice
activity detection, speaker identification and diarization, keyword
spotting, and more — all of which can slot into this plugin using the same
patterns.

- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) · [docs](https://k2-fsa.github.io/sherpa/onnx/) · [prebuilt models](https://github.com/k2-fsa/sherpa-onnx/releases)
