// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TTS/InoTTSTypes.h"

#include <atomic>

#include "InoTTSSubsystem.generated.h"

class FInoTtsEngine;

/**
 * On-device text-to-speech via sherpa-onnx (Piper/VITS today, Kokoro-ready).
 *
 * Threading contract (matches the InoAgents house style):
 *  - ALL public methods are game-thread-only.
 *  - ALL delegates fire on the game thread.
 *  - Async work runs on the engine ThreadPool; one synthesis in flight at
 *    a time (a second request errors immediately).
 *
 * Audio out is int16 mono PCM little-endian at the model's native rate
 * (FInoTTSResult::SampleRate). Playback is the caller's concern -- feed
 * RuntimeAudioImporter's UStreamingSoundWave or a USoundWaveProcedural;
 * UE's mixer resamples at the sink.
 *
 * Sync variants (LoadModel / Synthesize) BLOCK the calling thread --
 * meant for tooling, smoke tests, and short lines; gameplay uses the
 * async variants.
 */
UCLASS()
class INOSHERPA_API UInoTTS : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ UGameInstanceSubsystem
	virtual void Deinitialize() override;

	// ---- Model lifecycle -------------------------------------------------

	/** Loads a model on a worker thread; OnLoaded fires on the game thread. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS", meta = (AutoCreateRefTerm = "OnLoaded"))
	void LoadModelAsync(const FInoTTSModelConfig& Config, const FInoTTSLoadedDelegate& OnLoaded);

	/** Synchronous load; blocks the game thread (tooling / smoke tests). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS")
	bool LoadModel(const FInoTTSModelConfig& Config, FString& OutError);

	/** Cancels any in-flight synthesis and drops the model. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS")
	void UnloadModel();

	UFUNCTION(BlueprintPure, Category = "InoSherpa|TTS")
	bool IsModelLoaded() const;

	/** Native model output rate in Hz; 0 until a model is loaded. */
	UFUNCTION(BlueprintPure, Category = "InoSherpa|TTS")
	int32 GetSampleRate() const { return CachedSampleRate; }

	/** Speaker count of the loaded model (1 for single-speaker voices). */
	UFUNCTION(BlueprintPure, Category = "InoSherpa|TTS")
	int32 GetNumSpeakers() const { return CachedNumSpeakers; }

	UFUNCTION(BlueprintPure, Category = "InoSherpa|TTS")
	int32 GetNumChannels() const { return 1; }

	// ---- Synthesis -------------------------------------------------------

	/** Full-clip async synthesis; OnComplete fires on the game thread. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS", meta = (AutoCreateRefTerm = "OnComplete"))
	void SynthesizeAsync(const FString& Text, const FInoTTSOptions& Options,
		const FInoTTSCompleteDelegate& OnComplete);

	/**
	 * Streaming async synthesis: OnAudioChunk fires per generated chunk
	 * (int16 PCM at GetSampleRate()), then once more with an empty chunk
	 * and bIsFinal = true; OnComplete carries the full concatenated clip.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS", meta = (AutoCreateRefTerm = "OnAudioChunk,OnComplete"))
	void SynthesizeStreamAsync(const FString& Text, const FInoTTSOptions& Options,
		const FInoTTSAudioChunkDelegate& OnAudioChunk,
		const FInoTTSCompleteDelegate& OnComplete);

	/** Synchronous synthesis; blocks the game thread (tooling / short lines). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS")
	FInoTTSResult Synthesize(const FString& Text, const FInoTTSOptions& Options);

	// ---- Cancel / status ---------------------------------------------------

	/**
	 * Aborts the in-flight async synthesis mid-generation. OnComplete still
	 * fires, with the partial audio and bWasCancelled = true.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|TTS")
	void CancelSynthesis();

	UFUNCTION(BlueprintPure, Category = "InoSherpa|TTS")
	bool IsSynthInFlight() const { return bSynthInFlight; }

private:
	/** Shared body of the two async entry points. */
	void StartSynthesisInternal(const FString& Text, const FInoTTSOptions& Options,
		FInoTTSAudioChunkDelegate OnAudioChunk, bool bStreaming,
		FInoTTSCompleteDelegate OnComplete);

	/** Owns the sherpa handle; shared so workers outlive UnloadModel. */
	TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> Engine;

	/** Re-created per synthesis; polled by sherpa's progress callback. */
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CurrentCancelFlag;

	bool bIsLoading = false;
	bool bSynthInFlight = false;
	int32 CachedSampleRate = 0;
	int32 CachedNumSpeakers = 0;
};
