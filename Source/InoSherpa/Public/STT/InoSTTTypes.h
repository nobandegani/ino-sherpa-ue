// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sherpa/InoSherpaTypes.h"

#include "InoSTTTypes.generated.h"

/**
 * Streaming transducer (Zipformer) model configuration -- maps to sherpa's
 * SherpaOnnxOnlineRecognizerConfig with the transducer model family.
 * All paths are absolute filesystem paths, read once at load time.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoSTTModelConfig
{
	GENERATED_BODY()

	/** Transducer encoder .onnx. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	FString EncoderPath;

	/** Transducer decoder .onnx. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	FString DecoderPath;

	/** Transducer joiner .onnx. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	FString JoinerPath;

	/** tokens.txt shipped next to the model. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	FString TokensPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	int32 NumThreads = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	EInoSherpaProvider Provider = EInoSherpaProvider::Cpu;

	/** Verbose sherpa-side logging of the model load. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	bool bDebug = false;

	/**
	 * The model's expected feature sample rate (16000 for the published
	 * Zipformer models). Pushed audio at OTHER rates is fine -- sherpa
	 * resamples internally; this is the model-side constant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	int32 SampleRate = 16000;

	/** Feature (fbank) dimension; 80 for the published Zipformer models. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	int32 FeatureDim = 80;

	/** "greedy_search" (default) or "modified_beam_search". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	FString DecodingMethod = TEXT("greedy_search");

	/** Beam width when DecodingMethod is modified_beam_search. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	int32 MaxActivePaths = 4;

	/**
	 * Endpoint detection: when enabled the stream auto-segments utterances
	 * (final result + endpoint event + decoder reset) using the three rules
	 * below. Rule 1: trailing silence with NO decoded text yet. Rule 2:
	 * trailing silence AFTER some text. Rule 3: hard utterance-length cap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	bool bEnableEndpoint = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	float Rule1MinTrailingSilence = 2.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	float Rule2MinTrailingSilence = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoSherpa|STT")
	float Rule3MinUtteranceLength = 20.0f;
};

/** A recognition result -- partial (bIsFinal=false) or utterance-final. */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoSTTResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|STT")
	FString Text;

	/** True for endpoint-segmented / finished-stream / one-shot results. */
	UPROPERTY(BlueprintReadOnly, Category = "InoSherpa|STT")
	bool bIsFinal = false;
};

/** Model load finished (fires on the game thread). */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoSTTLoadedDelegate, bool, bSuccess, FString, ErrorMessage);

/** Growing partial hypothesis for the current utterance. Game thread. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoSTTResultDelegate, const FInoSTTResult&, Result);

/** Utterance-final result (endpoint, FinishStream, or one-shot). Game thread. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoSTTFinalDelegate, const FInoSTTResult&, Result);

/** Payload-less endpoint cue (fires right after the final it segments). Game thread. */
DECLARE_DYNAMIC_DELEGATE(FInoSTTEndpointDelegate);
