// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STT/InoSTTTypes.h"

typedef struct SherpaOnnxOfflineRecognizer SherpaOnnxOfflineRecognizer;

/**
 * Module-private owner of one sherpa-onnx OfflineRecognizer handle (a
 * non-streaming, whole-utterance model -- Parakeet-TDT et al). Shared:
 * in-flight transcribe workers hold TSharedPtr copies so the handle
 * outlives a concurrent UnloadOfflineModel.
 *
 * A separate ORT session from the streaming recognizer, so offline
 * one-shots may run alongside a live streaming session. Offline decodes
 * are still serialized among THEMSELVES (UInoSTT's
 * bOfflineTranscribeInFlight) -- same one-decode-per-recognizer
 * conservatism as the online side.
 */
class FInoSttOfflineRecognizer
{
public:
	/** Blocking; run on a worker thread (0.6B models take a few seconds). */
	static TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> Create(const FInoSTTOfflineModelConfig& Config, FString& OutError);

	~FInoSttOfflineRecognizer();

	FInoSttOfflineRecognizer(const FInoSttOfflineRecognizer&) = delete;
	FInoSttOfflineRecognizer& operator=(const FInoSttOfflineRecognizer&) = delete;

	/**
	 * Whole-utterance transcription: one throwaway offline stream, whole
	 * buffer in, text out. Blocking. Sherpa resamples internally, so any
	 * input rate works (no tail padding needed offline -- the decoder sees
	 * the full clip with complete context).
	 */
	FString Transcribe(TArrayView<const float> Samples, int32 SampleRate) const;

private:
	explicit FInoSttOfflineRecognizer(const SherpaOnnxOfflineRecognizer* InRecognizer)
		: Recognizer(InRecognizer)
	{
	}

	const SherpaOnnxOfflineRecognizer* Recognizer = nullptr;
};
