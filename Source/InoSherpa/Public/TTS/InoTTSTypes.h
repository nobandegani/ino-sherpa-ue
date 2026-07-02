// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sherpa/InoSherpaTypes.h"

#include "InoTTSTypes.generated.h"

/**
 * Piper / VITS model paths + knobs (maps to sherpa's
 * SherpaOnnxOfflineTtsVitsModelConfig). All paths are absolute filesystem
 * paths, read once at load time.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoTTSPiperConfig
{
	GENERATED_BODY()

	/** The VITS .onnx model file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString ModelPath;

	/** tokens.txt shipped next to the model. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString TokensPath;

	/**
	 * espeak-ng-data directory (ships inside Piper model bundles). Piper
	 * phonemization REQUIRES this unless LexiconPath is provided instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString DataDir;

	/** Alternative to DataDir for lexicon-based models. Usually empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString LexiconPath;

	/** VITS noise scale (expressiveness of the generated speech). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float NoiseScale = 0.667f;

	/** VITS duration-predictor noise scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float NoiseScaleW = 0.8f;

	/** Phoneme duration scale; >1 = slower speech. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float LengthScale = 1.0f;
};

/**
 * Kokoro model paths + knobs (maps to sherpa's
 * SherpaOnnxOfflineTtsKokoroModelConfig). Plumbed through now so Kokoro
 * drops in without a types churn; engine wiring lands after Piper.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoTTSKokoroConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString ModelPath;

	/** voices.bin — Kokoro is multi-speaker; select via FInoTTSOptions::SpeakerId. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString VoicesPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString TokensPath;

	/** espeak-ng-data directory (Kokoro English uses espeak phonemization). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString DataDir;

	/** Optional lexicon (multi-language Kokoro variants). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString LexiconPath;

	/** Optional language hint (multi-language Kokoro variants). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString Lang;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float LengthScale = 1.0f;
};

/**
 * Load-time TTS configuration (maps to SherpaOnnxOfflineTtsConfig).
 * Set ModelType and fill the matching sub-struct; the other is ignored.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoTTSModelConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	EInoTTSModelType ModelType = EInoTTSModelType::PiperVits;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FInoTTSPiperConfig Piper;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FInoTTSKokoroConfig Kokoro;

	/** ORT intra-op threads for this model. 1 is plenty for Piper-class models. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	int32 NumThreads = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	EInoSherpaProvider Provider = EInoSherpaProvider::Cpu;

	/** Verbose sherpa-side logging of the model load. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	bool bDebug = false;

	/**
	 * Max sentences per internal batch. 1 = lowest latency to first audio
	 * chunk when streaming (sherpa splits input text into sentences).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	int32 MaxNumSentences = 1;

	/** Scale applied to inter-sentence silence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float SilenceScale = 0.2f;

	/** Optional rule FST paths (comma-separated) for text normalization. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	FString RuleFsts;
};

/** Per-utterance generation options (maps to SherpaOnnxGenerationConfig). */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoTTSOptions
{
	GENERATED_BODY()

	/** Speech speed multiplier; >1 = faster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float Speed = 1.0f;

	/** Speaker id for multi-speaker models (Kokoro, multi-speaker VITS). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	int32 SpeakerId = 0;

	/** Scale applied to inter-sentence silence for this utterance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|TTS")
	float SilenceScale = 0.2f;
};

/**
 * A finished (or cancelled) synthesis.
 *
 * AudioSamples is 16-bit signed mono PCM, little-endian, at SampleRate --
 * the plugin family wire format. Feed it to RuntimeAudioImporter's
 * UStreamingSoundWave (AppendAudioDataFromRAW, Int16) or any
 * USoundWaveProcedural; UE's mixer handles rate conversion, so no
 * resampling here.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoTTSResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	bool bSuccess = false;

	/** Set when bSuccess is false. Cancelled runs report success=true with partial audio. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	FString ErrorMessage;

	/** int16 mono PCM LE. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	TArray<uint8> AudioSamples;

	/** Native model rate (22050 for Piper medium voices). */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	int32 SampleRate = 0;

	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	int32 NumChannels = 1;

	/** Duration of AudioSamples in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	float DurationSeconds = 0.0f;

	/** Wall-clock generation time in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	float GenerationTimeSeconds = 0.0f;

	/** GenerationTime / Duration; <1 means faster than real time. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	float RealTimeFactor = 0.0f;

	/** True when the run was cut short by CancelSynthesis / teardown. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|TTS")
	bool bWasCancelled = false;
};

/** Model load finished (fires on the game thread). */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoTTSLoadedDelegate, bool, bSuccess, FString, ErrorMessage);

/** Synthesis finished -- full audio (or partial audio on cancel). Game thread. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoTTSCompleteDelegate, const FInoTTSResult&, Result);

/**
 * Streaming audio chunk (int16 mono PCM LE at the model's native rate).
 * Progress is sherpa's [0,1] estimate; bIsFinal accompanies an EMPTY chunk
 * fired after the last audio chunk. Game thread.
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FInoTTSAudioChunkDelegate, const TArray<uint8>&, AudioChunk, float, Progress, bool, bIsFinal);
