// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TTS/InoTTSTypes.h"
#include "UObject/Object.h"

#include "InoSherpaTtsSmokeHelper.generated.h"

/**
 * Dynamic-delegate landing pad for the Ino.Sherpa.TTS.* smoke tests
 * (single-cast dynamic delegates can only bind UFUNCTIONs). Kept alive by
 * a TStrongObjectPtr in the smoke-test TU while a test is in flight.
 */
UCLASS()
class UInoSherpaTtsSmokeHelper : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleChunk(const TArray<uint8>& AudioChunk, float Progress, bool bIsFinal);

	UFUNCTION()
	void HandleComplete(const FInoTTSResult& Result);

	/** Where HandleComplete writes the WAV ("" = skip). */
	FString WavOutPath;

	/** CancelTest sets this: completion must report bWasCancelled. */
	bool bExpectCancelled = false;

	int32 NumChunks = 0;
	int64 NumChunkBytes = 0;
};
