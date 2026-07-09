// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STT/InoSTTTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "InoSTTSubsystem.generated.h"

class FInoCancellationToken;
class FInoSttOfflineRecognizer;
class FInoSttRecognizer;
class FInoSttStreamWorker;

/**
 * On-device speech-to-text via sherpa-onnx (streaming Zipformer transducer).
 *
 * PUSH-ONLY input: the caller feeds mono PCM buffers (int16 bytes or
 * float32) from any source -- microphone capture is deliberately not this
 * subsystem's concern. Any sample rate is accepted; sherpa resamples
 * internally to the model's rate.
 *
 * Streaming session flow:
 *   StartStream(OnPartial, OnFinal, OnEndpoint)
 *   PushAudioInt16/Float(...)   repeatedly
 *     -> OnPartial fires with the growing hypothesis
 *     -> on a detected endpoint (pause): OnFinal + OnEndpoint fire and the
 *        recognizer auto-resets for the next utterance
 *   FinishStream()               flushes the tail -> OnFinal
 *   StopStream()                 tears the session down (model stays loaded)
 *
 * Threading contract (matches the InoAgents house style): all public
 * methods game-thread-only; all delegates fire on the game thread. A
 * dedicated worker thread owns the sherpa stream (its API demands
 * single-thread access per stream).
 *
 * One-shot Transcribe* variants exist for whole clips; they cannot run
 * while a streaming session is active (single-decode-at-a-time invariant).
 */
UCLASS()
class INOSHERPA_API UInoSTT : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ UGameInstanceSubsystem
	virtual void Deinitialize() override;

	// ---- Model lifecycle -------------------------------------------------

	/** Loads a model on a worker thread; OnLoaded fires on the game thread. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnLoaded"))
	void LoadModelAsync(const FInoSTTModelConfig& Config, const FInoSTTLoadedDelegate& OnLoaded);

	/** Synchronous load; blocks the game thread (tooling / smoke tests). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	bool LoadModel(const FInoSTTModelConfig& Config, FString& OutError);

	/** Stops any streaming session and drops the model. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void UnloadModel();

	UFUNCTION(BlueprintPure, Category = "InoSherpa|STT")
	bool IsModelLoaded() const;

	// ---- Streaming session (push-only) ------------------------------------

	/** Begins a streaming session. Delegates fire on the game thread. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnPartial,OnFinal,OnEndpoint"))
	void StartStream(const FInoSTTResultDelegate& OnPartial,
		const FInoSTTFinalDelegate& OnFinal,
		const FInoSTTEndpointDelegate& OnEndpoint);

	/** Push int16 mono PCM LE bytes at the CALLER's sample rate. Any rate
	 *  works, but it must stay THE SAME for the whole session -- chunks at
	 *  a different rate are dropped with an error (sherpa locks a stream's
	 *  resampler to the first rate it sees). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void PushAudioInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate);

	/** Push float32 mono samples [-1,1] at the CALLER's sample rate. Same
	 *  keep-the-rate-consistent rule as PushAudioInt16. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void PushAudioFloat(const TArray<float>& Samples, int32 SampleRate);

	/** Ends the current utterance: flushes the decoder tail -> OnFinal.
	 *  The session stays alive for the next utterance. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void FinishStream();

	/** Forces an utterance boundary now (pending text -> OnFinal). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void ResetStream();

	/** Tears down the streaming session; the model stays loaded. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void StopStream();

	UFUNCTION(BlueprintPure, Category = "InoSherpa|STT")
	bool IsStreaming() const;

	// ---- One-shot (whole utterance) ---------------------------------------

	/** Synchronous one-shot; blocks the game thread. Not while streaming. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	FInoSTTResult TranscribeInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate);

	/** Synchronous one-shot; blocks the game thread. Not while streaming. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	FInoSTTResult TranscribeFloat(const TArray<float>& Samples, int32 SampleRate);

	/** Async one-shot; OnComplete fires on the game thread. Not while streaming. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnComplete"))
	void TranscribeAsync(const TArray<float>& Samples, int32 SampleRate,
		const FInoSTTFinalDelegate& OnComplete);

	// ---- Offline (non-streaming) model: higher accuracy, whole clips ------
	// A SECOND, independent model (e.g. Parakeet-TDT) that can be loaded
	// alongside the streaming one. Offline transcribes may run while a
	// streaming session is live (separate inference sessions); they only
	// serialize against each other.

	/** Loads the offline model on a worker thread (a few seconds for 0.6B). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnLoaded"))
	void LoadOfflineModelAsync(const FInoSTTOfflineModelConfig& Config, const FInoSTTLoadedDelegate& OnLoaded);

	/** Synchronous offline-model load; blocks the game thread (tooling). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	bool LoadOfflineModel(const FInoSTTOfflineModelConfig& Config, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void UnloadOfflineModel();

	UFUNCTION(BlueprintPure, Category = "InoSherpa|STT")
	bool IsOfflineModelLoaded() const;

	/** Synchronous offline one-shot; blocks the game thread. Any sample rate. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	FInoSTTResult TranscribeOfflineInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate);

	/** Synchronous offline one-shot; blocks the game thread. Any sample rate. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	FInoSTTResult TranscribeOfflineFloat(const TArray<float>& Samples, int32 SampleRate);

	/** Async offline one-shot; OnComplete fires on the game thread. */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnComplete"))
	void TranscribeOfflineAsync(const TArray<float>& Samples, int32 SampleRate,
		const FInoSTTFinalDelegate& OnComplete);

	// ---- Settings-driven download + load -----------------------------------
	// Model sources live in Project Settings -> Plugins -> InoSherpa. These
	// download whatever is missing (InoNodes: cached-skip, resume, retries,
	// optional SHA-256) and then load the model. OnDownloadProgress fires per
	// tick on the game thread (OverallProgressPercent spans all files);
	// OnLoaded fires exactly once. Already-downloaded models skip straight
	// to the load.

	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnDownloadProgress,OnLoaded"))
	void LoadStreamingModelFromSettingsAsync(const FInoSTTDownloadProgressDelegate& OnDownloadProgress,
		const FInoSTTLoadedDelegate& OnLoaded);

	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT", meta = (AutoCreateRefTerm = "OnDownloadProgress,OnLoaded"))
	void LoadOfflineModelFromSettingsAsync(const FInoSTTDownloadProgressDelegate& OnDownloadProgress,
		const FInoSTTLoadedDelegate& OnLoaded);

	/** Cheap file-stat probe (no hashing) -- safe to poll from UMG. */
	UFUNCTION(BlueprintPure, Category = "InoSherpa|STT")
	bool IsStreamingModelDownloaded() const;

	UFUNCTION(BlueprintPure, Category = "InoSherpa|STT")
	bool IsOfflineModelDownloaded() const;

	/** Cancels an in-flight settings-driven download (OnLoaded fires with failure). */
	UFUNCTION(BlueprintCallable, Category = "InoSherpa|STT")
	void CancelModelDownload();

	// ---- Worker -> game-thread notifications (internal; do not call) ------
	// SessionSerial guards against events queued by a previous session
	// arriving after a same-frame StopStream -> StartStream.

	void NotifyPartialFromWorker(const FString& Text, int32 SessionSerial);
	void NotifyFinalFromWorker(const FString& Text, int32 SessionSerial);
	void NotifyEndpointFromWorker(int32 SessionSerial);
	void NotifyWorkerDiedFromWorker(int32 SessionSerial);

private:
	/** Owns the sherpa recognizer; shared so workers outlive UnloadModel. */
	TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> Recognizer;

	/** The offline (non-streaming) recognizer; independent of the above. */
	TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> OfflineRecognizer;

	/** Owns the sherpa stream + its dedicated thread (TSharedPtr so the
	 *  incomplete type is deletable from the generated code). */
	TSharedPtr<FInoSttStreamWorker> StreamWorker;

	// Session delegates; only touched on the game thread.
	FInoSTTResultDelegate OnPartialDelegate;
	FInoSTTFinalDelegate OnFinalDelegate;
	FInoSTTEndpointDelegate OnEndpointDelegate;

	/** Bumped on every StartStream/StopStream; stamps worker dispatches. */
	int32 StreamSessionSerial = 0;

	/** Cancel signal for the in-flight settings-driven download, if any. */
	TSharedPtr<FInoCancellationToken, ESPMode::ThreadSafe> ActiveDownloadToken;

	bool bIsLoading = false;
	bool bTranscribeInFlight = false;
	bool bOfflineLoading = false;
	bool bOfflineTranscribeInFlight = false;
};
