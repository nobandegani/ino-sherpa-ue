// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CoreMinimal.h"
#include "TTS/InoTTSTypes.h"

#include <atomic>

typedef struct SherpaOnnxOfflineTts SherpaOnnxOfflineTts;

/**
 * Module-private owner of one sherpa-onnx OfflineTts handle.
 *
 * NOT a UObject: an in-flight ThreadPool worker holds a TSharedPtr copy so
 * the sherpa handle safely outlives a concurrent UnloadModel / subsystem
 * Deinitialize. No internal locking -- UInoTTS's single-op-in-flight
 * invariant (bSynthInFlight) guarantees Generate runs on one thread at a
 * time.
 */
class FInoTtsEngine
{
public:
	/**
	 * Streaming sink, invoked on the GENERATING thread with each freshly
	 * converted int16 chunk. The subsystem bridges to the game thread.
	 */
	using FChunkFn = TFunction<void(TArray<uint8>&& Int16PcmBytes, float Progress)>;

	/** Blocking; run on a worker thread. Null + OutError on bad config. */
	static TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> Create(const FInoTTSModelConfig& Config, FString& OutError);

	~FInoTtsEngine();

	FInoTtsEngine(const FInoTtsEngine&) = delete;
	FInoTtsEngine& operator=(const FInoTtsEngine&) = delete;

	int32 GetSampleRate() const;
	int32 GetNumSpeakers() const;

	/**
	 * Blocking synthesis via SherpaOnnxOfflineTtsGenerateWithConfig.
	 *
	 * CancelFlag (optional): polled in sherpa's progress callback; a set
	 * flag aborts generation mid-run (the callback returns 0) and the
	 * result carries the partial audio with bWasCancelled = true.
	 * ChunkFn (optional): streaming sink, see above.
	 */
	FInoTTSResult Generate(const FString& Text, const FInoTTSOptions& Options,
		const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancelFlag,
		const FChunkFn& ChunkFn) const;

private:
	explicit FInoTtsEngine(const SherpaOnnxOfflineTts* InTts)
		: Tts(InTts)
	{
	}

	const SherpaOnnxOfflineTts* Tts = nullptr;
};
