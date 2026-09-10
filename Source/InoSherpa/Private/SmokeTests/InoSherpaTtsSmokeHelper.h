// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
