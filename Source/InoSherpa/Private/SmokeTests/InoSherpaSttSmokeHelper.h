// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STT/InoSTTTypes.h"
#include "UObject/Object.h"

#include "InoSherpaSttSmokeHelper.generated.h"

/**
 * Dynamic-delegate landing pad for the Ino.Sherpa.STT.* smoke tests.
 * Kept alive by a TStrongObjectPtr in the smoke-test TU.
 */
UCLASS()
class UInoSherpaSttSmokeHelper : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandlePartial(const FInoSTTResult& Result);

	UFUNCTION()
	void HandleFinal(const FInoSTTResult& Result);

	UFUNCTION()
	void HandleEndpoint();

	UFUNCTION()
	void HandleLoaded(bool bSuccess, FString ErrorMessage);

	UFUNCTION()
	void HandleDownloadProgress(const FInoDownloadProgress& Progress);

	int32 NumPartials = 0;
	int32 NumFinals = 0;
	int32 NumEndpoints = 0;

	// Download-progress log throttle state.
	int32 LastLoggedTenPercent = -1;
	int32 LastLoggedFileIndex = -1;
};
