// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "InoSherpaTypes.generated.h"

/**
 * Execution provider for sherpa-onnx inference sessions.
 *
 * Phase 1 is CPU-only by construction: the statically linked ONNX Runtime
 * inside this module is upstream's CPU build (that is all their static
 * release packages contain). The enum exists so accelerated providers can
 * slot in later without a signature change.
 */
UENUM(BlueprintType)
enum class EInoSherpaProvider : uint8
{
	Cpu UMETA(DisplayName = "CPU"),
};

/** Which TTS model family a FInoTTSModelConfig describes. */
UENUM(BlueprintType)
enum class EInoTTSModelType : uint8
{
	/** Piper / VITS voices (phase 1 target; needs espeak-ng-data). */
	PiperVits UMETA(DisplayName = "Piper (VITS)"),

	/** Kokoro multi-speaker voices (config plumbed now, wired next). */
	Kokoro UMETA(DisplayName = "Kokoro"),
};

namespace InoSherpa
{
/** Maps EInoSherpaProvider to the string sherpa-onnx configs expect. */
inline const char* ProviderToString(EInoSherpaProvider Provider)
{
	switch (Provider)
	{
	case EInoSherpaProvider::Cpu:
	default:
		return "cpu";
	}
}
} // namespace InoSherpa
