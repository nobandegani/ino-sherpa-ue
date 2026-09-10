// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CoreMinimal.h"
#include "STT/InoSTTTypes.h"

typedef struct SherpaOnnxOnlineRecognizer SherpaOnnxOnlineRecognizer;

/**
 * Module-private owner of one sherpa-onnx OnlineRecognizer handle (the
 * loaded streaming model). Shared: the stream worker and one-shot
 * transcribe workers hold TSharedPtr copies so the handle outlives a
 * concurrent UnloadModel.
 *
 * The recognizer itself is read-only model state; all MUTABLE decoding
 * state lives in per-stream objects. sherpa has no internal locking, so
 * every stream must be driven by exactly one thread at a time -- and we
 * additionally never decode two streams concurrently (see UInoSTT's
 * single-session invariants).
 */
class FInoSttRecognizer
{
public:
	/** Blocking; run on a worker thread. Null + OutError on bad config. */
	static TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> Create(const FInoSTTModelConfig& Config, FString& OutError);

	~FInoSttRecognizer();

	FInoSttRecognizer(const FInoSttRecognizer&) = delete;
	FInoSttRecognizer& operator=(const FInoSttRecognizer&) = delete;

	const SherpaOnnxOnlineRecognizer* GetHandle() const { return Recognizer; }

	/** The model's feature sample rate (from the load config). */
	int32 GetFeatSampleRate() const { return FeatSampleRate; }

	/**
	 * Whole-utterance one-shot on a throwaway stream: accept the full
	 * buffer, signal input-finished, pump the decoder dry, return the text.
	 * Blocking; caller guarantees no other stream of this recognizer is
	 * being decoded concurrently.
	 */
	FString TranscribeOnce(TArrayView<const float> Samples, int32 SampleRate) const;

private:
	FInoSttRecognizer(const SherpaOnnxOnlineRecognizer* InRecognizer, int32 InFeatSampleRate)
		: Recognizer(InRecognizer)
		, FeatSampleRate(InFeatSampleRate)
	{
	}

	const SherpaOnnxOnlineRecognizer* Recognizer = nullptr;
	int32 FeatSampleRate = 16000;
};
