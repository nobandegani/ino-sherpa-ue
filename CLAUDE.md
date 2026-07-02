# CLAUDE.md — InoSherpa plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoSherpa/`. The hosting demo project is documented in
the project root's `CLAUDE.md`.

## Purpose

`InoSherpa` is an Unreal Engine 5.8 runtime plugin whose job is to
**integrate k2-fsa's sherpa-onnx speech toolkit and expose it as a UE
module** that other plugins (eventually `InoAgents`) declare as a
dependency. sherpa-onnx provides on-device, offline speech capabilities
on top of ONNX Runtime:

- **TTS** — Piper (VITS), Kokoro, Matcha-TTS, and others, including the
  full text frontend (espeak-ng phonemization, G2P).
- **ASR** — streaming + non-streaming (Zipformer, Whisper, Paraformer,
  SenseVoice, Moonshine).
- **Extras** — VAD (Silero/TEN), speaker ID/diarization, keyword
  spotting, punctuation restoration, audio tagging, speech enhancement.

Upstream: https://github.com/k2-fsa/sherpa-onnx

## Status / roadmap

**Windows-first.** The plugin is being brought up on Win64 only; other
platforms follow after Windows is fully finished. Every architectural
decision, however, is made with all target platforms in mind — the
per-platform strategy below is settled, only the implementation is
phased.

1. ⏳ **Phase 1 — Win64** (in progress): setup script staging the
   official static release libs, `Build.cs` wiring, module skeleton,
   smoke test, first TTS/ASR API surface.
2. ⏳ **Phase 2 — Android** (arm64-v8a): self-built static libs from
   `Vendor/sherpa-onnx` (see "Why Android is self-built").
3. ⏳ **Phase 3 — iOS / macOS** as project needs dictate: official
   static release artifacts.

Present state: empty module skeleton (template `InoSherpa.h/.cpp`) +
the vendored upstream submodule. Nothing is wired yet.

## The one core decision: static everywhere

sherpa-onnx bundles its **own** ONNX Runtime (their build, CPU-only).
The engine process is already crowded with ONNX Runtimes: UE's NNE
plugin ships `onnxruntime.dll` (1.19.x), marketplace plugins ship
their own, and our sibling `InoOnnx` stages a renamed DirectML build
(`InoOnnxRuntime.dll` / `libInoOnnxRuntime.so` — see
`Plugins/InoOnnx/CLAUDE.md` "Why we rename the DLLs / .so" for the
full failure-mode catalog: Windows base-name DLL cache, Android SONAME
aliasing).

sherpa-onnx's **shared** release packages ship a plain-named
`onnxruntime.dll` / `libonnxruntime.so` next to the sherpa libs — that
would re-create exactly the collisions InoOnnx's renames solved.

**Therefore InoSherpa links sherpa-onnx and its ONNX Runtime
STATICALLY on every platform.** All of sherpa + its ORT ends up inside
our own uniquely-named module binary with no exports; no plain-named
ORT binary is ever shipped. Collisions become structurally impossible
without any rename surgery.

Consequences:

- InoSherpa's ORT is CPU-only (that's all upstream links into their
  static packages). Fine for speech workloads. `InoOnnx` remains the
  accelerated (DirectML) path for everything else — the two ORT copies
  coexist because neither exports nor ships a plain-named binary.
- No `PreLoadingScreen` loading phase needed, unlike the DLL-staging
  siblings (`InoOnnx`, `InoLlama`, `InoLiteRT`) — there is no separate
  runtime binary to pre-load. `LoadingPhase=Default` is correct here.

## Per-platform artifact strategy

Verified against the v1.13.3 release (2026-06-15, 294 assets):

| Platform | Source of static libs | Notes |
|---|---|---|
| **Win64** (phase 1) | Official release: `sherpa-onnx-v<ver>-win-x64-static-MD-Release-lib.tar.bz2` | 14 `.lib` files incl. static `onnxruntime.lib` (ORT 1.24.4, their build), espeak-ng, kaldi-fbank, piper-phonemize, ssentencepiece, ucd, kissfft. **MD** CRT matches UE's `/MD`. No headers in the archive — headers come from `Vendor/sherpa-onnx`. |
| **Android** arm64-v8a (phase 2) | **Self-built** from `Vendor/sherpa-onnx` | No usable static release exists (see below). Build with `SHERPA_ONNX_ENABLE_C_API=ON`, `SHERPA_ONNX_ENABLE_JNI=OFF`, static ORT (`BUILD_SHARED_LIBS=OFF` path pulls `onnxruntime-android-arm64-v8a-static_lib`). |
| **iOS** (phase 3) | Official release: `sherpa-onnx-v<ver>-ios.tar.bz2` | `sherpa-onnx.xcframework` with merged `libsherpa-onnx.a` (+ C API headers) plus a **separate** static `onnxruntime.xcframework` (ORT 1.26.0). ⚠ InoOnnx on iOS also statically links its ORT (1.24.3) into the main executable — shipping both plugins on iOS requires unifying on ONE ORT static archive or the link fails on duplicate `Ort*` symbols. Decide when iOS enters scope. |
| **macOS** (phase 3) | Official release: `osx-arm64-static` / `osx-universal2-static` packages | Same shape as Win64 static. |
| Linux | Official release: `linux-x64-static` packages | Not a project target; slots in if ever needed. |

Why Android is self-built: the release has no static-libs package for
Android. The only "static ORT" Android artifact
(`android-static-link-onnxruntime.tar.bz2`) contains **only
`libsherpa-onnx-jni.so`** — JNI for Kotlin/Java apps, no
`libsherpa-onnx-c-api.so` — unusable from UE native code. The regular
`android.tar.bz2` has the C API lib but next to a plain-named
`libonnxruntime.so` (SONAME-aliasing hazard). Upstream's
`build-android-arm64-v8a.sh` supports exactly the combination we need;
it requires an Android NDK (point `ANDROID_NDK` at the one UE 5.8
uses) and a bash-capable shell.

## Version pinning

- Pinned upstream version: **v1.13.3** (record in a `SHERPA_VERSION`
  file once the setup script lands, following the sibling-plugin
  pattern).
- Upstream has **no release branch** — development happens on `master`
  and version tags are cut from it. Prebuilt release assets exist only
  for tags.
- `Vendor/sherpa-onnx` (git submodule) must be pinned to the **same
  tag** as the release artifacts we stage, because:
  1. C API headers (`sherpa-onnx/c-api/c-api.h`, `cxx-api.h`) are
     consumed from the submodule — Win64 `-lib` release archives ship
     no headers.
  2. Android builds compile the submodule source directly.
  The setup script should enforce tag == pinned version, the way
  `InoLlama`'s does. (Known drift right now: the submodule sits on
  `master @ ca668535`, 15 commits past v1.13.3 — re-pin when the setup
  script lands.)
- sherpa's ORT builds come from
  https://github.com/csukuangfj/onnxruntime-libs (their own ORT
  builds); the version is dictated by the sherpa release / cmake files,
  not by us. It intentionally does NOT need to match InoOnnx's pinned
  ORT — the copies are isolated.

## Layout

```
Plugins/InoSherpa/
├── CLAUDE.md                        ← this file
├── InoSherpa.uplugin                ← Runtime module, LoadingPhase=Default
├── Vendor/
│   └── sherpa-onnx/                 ← upstream git submodule (headers for
│                                      all platforms; source for Android
│                                      self-build). Pin = release tag.
└── Source/
    └── InoSherpa/
        ├── InoSherpa.Build.cs
        ├── Public/InoSherpa.h       ← template skeleton (to be replaced)
        └── Private/InoSherpa.cpp    ← template skeleton (to be replaced)
```

Planned additions as phase 1 lands (naming follows `InoOnnx` /
`InoLlama` conventions):

```
├── SherpaOnnx/                      ← setup workspace
│   ├── SHERPA_VERSION               ← pinned upstream tag (e.g. "1.13.3")
│   ├── scripts/setup-sherpa-onnx.ps1← downloads + stages release static libs,
│   │                                  enforces Vendor submodule tag match
│   └── .cache/                      ← downloaded archives (gitignored)
└── Source/
    └── ThirdParty/
        ├── Win64/                   ← staged .lib files (GITIGNORED — ~771 MB
        │                              unpacked, onnxruntime.lib alone is 685 MB,
        │                              over GitHub's 100 MB limit; the setup
        │                              script is the restore path)
        └── Android/ / IOS/ / Mac/   ← later phases
```

## Naming

All plugin-facing types are `Ino`-prefixed (`FInoSherpaModule`,
`FInoSherpaTts…`, `EInoSherpa…`), matching the rule enforced across
the sibling plugins. Upstream symbols stay behind the module boundary;
consumers never include sherpa headers directly.

## How other plugins will consume this (planned)

Same pattern as the siblings:

```csharp
PublicDependencyModuleNames.Add("InoSherpa");
```

```json
"Plugins": [ { "Name": "InoSherpa", "Enabled": true } ]
```

Consumer-facing API surface (TTS first, ASR later) will be a C++
wrapper in `Public/` — shape TBD in phase 1; look at
`InoOnnx`'s two-level API (raw + RAII wrapper) for the house style.

## Git hygiene

This directory is its **own git repository** (vendored-but-live inside
the demo project, like every `Plugins/<Name>/`). Commits go to this
repo with `git -C Plugins/InoSherpa …`, never through the project
repo. `Vendor/sherpa-onnx` is a submodule of THIS repo; fresh clones
need `git submodule update --init`.

## Authoritative references

- sherpa-onnx repo: https://github.com/k2-fsa/sherpa-onnx
- Releases (prebuilt artifacts): https://github.com/k2-fsa/sherpa-onnx/releases
- Docs: https://k2-fsa.github.io/sherpa/onnx/
- C API header (in submodule): `Vendor/sherpa-onnx/sherpa-onnx/c-api/c-api.h`
- Upstream Android build script: `Vendor/sherpa-onnx/build-android-arm64-v8a.sh`
- Their ORT builds: https://github.com/csukuangfj/onnxruntime-libs
- Sibling plugin docs this file leans on:
  - `Plugins/InoOnnx/CLAUDE.md` — the ORT-collision failure catalog + rename pattern we deliberately avoid needing
  - `Plugins/InoLlama/CLAUDE.md` — the setup-script / version-pin / vendor-submodule-sync pattern to copy
