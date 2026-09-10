// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CoreMinimal.h"

/**
 * Module-private PCM helpers.
 *
 * Mirrors the shapes of InoAgents' UInoAudioFunctionLibrary converters
 * WITHOUT depending on InoAgents (runtime plugins never depend on their
 * consumers). int16 mono PCM little-endian is the cross-plugin wire
 * format; float32 mono [-1,1] is what sherpa-onnx produces/consumes.
 */
namespace InoSherpaPcm
{
/** Clamp to [-1,1], scale to int16, append as little-endian bytes. */
void Float32ToInt16PcmBytesMono(TArrayView<const float> Samples, TArray<uint8>& OutBytes);

/** Reinterpret int16 LE bytes as float32 [-1,1]. False + error on odd byte count. */
bool Int16PcmBytesToFloat32Mono(TArrayView<const uint8> PcmBytes, TArray<float>& OutSamples, FString* OutError = nullptr);

/**
 * Minimal RIFF/WAVE writer (PCM16 mono) for smoke tests / debugging.
 * Creates parent directories as needed.
 */
bool WriteInt16PcmBytesAsWav(const FString& Path, TArrayView<const uint8> PcmBytes, int32 SampleRate, FString* OutError = nullptr);

/**
 * Minimal RIFF/WAVE reader for smoke tests: accepts mono PCM16 or mono
 * IEEE float32, returns float32 [-1,1] + the file's sample rate. Strict
 * on purpose -- anything else errors with a clear message.
 */
bool ReadMonoWavAsFloat32(const FString& Path, TArray<float>& OutSamples, int32& OutSampleRate, FString* OutError = nullptr);
} // namespace InoSherpaPcm
