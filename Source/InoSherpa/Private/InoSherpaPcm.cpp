// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "InoSherpaPcm.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace InoSherpaPcm
{

// Scale convention: encode x32767, decode /32768 -- matches InoAgents'
// UInoAudioFunctionLibrary. Round-trip of exactly -32768 loses one LSB;
// irrelevant for speech, kept for cross-plugin consistency.
void Float32ToInt16PcmBytesMono(TArrayView<const float> Samples, TArray<uint8>& OutBytes)
{
	const int32 FirstByte = OutBytes.Num();
	OutBytes.AddUninitialized(Samples.Num() * 2);
	uint8* Dst = OutBytes.GetData() + FirstByte;
	for (const float Sample : Samples)
	{
		const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
		const int16 Value = static_cast<int16>(FMath::RoundToInt(Clamped * 32767.0f));
		*Dst++ = static_cast<uint8>(Value & 0xFF);
		*Dst++ = static_cast<uint8>((Value >> 8) & 0xFF);
	}
}

bool Int16PcmBytesToFloat32Mono(TArrayView<const uint8> PcmBytes, TArray<float>& OutSamples, FString* OutError)
{
	if ((PcmBytes.Num() % 2) != 0)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("int16 PCM byte buffer has odd length %d"), PcmBytes.Num());
		}
		return false;
	}
	const int32 NumSamples = PcmBytes.Num() / 2;
	OutSamples.Reset(NumSamples);
	OutSamples.AddUninitialized(NumSamples);
	const uint8* Src = PcmBytes.GetData();
	for (int32 i = 0; i < NumSamples; ++i)
	{
		const int16 Value = static_cast<int16>(static_cast<uint16>(Src[0]) | (static_cast<uint16>(Src[1]) << 8));
		OutSamples[i] = static_cast<float>(Value) / 32768.0f;
		Src += 2;
	}
	return true;
}

bool WriteInt16PcmBytesAsWav(const FString& Path, TArrayView<const uint8> PcmBytes, int32 SampleRate, FString* OutError)
{
	if (SampleRate <= 0)
	{
		if (OutError) { *OutError = FString::Printf(TEXT("invalid sample rate %d"), SampleRate); }
		return false;
	}

	const uint32 DataSize = static_cast<uint32>(PcmBytes.Num());
	const uint16 NumChannels = 1;
	const uint16 BitsPerSample = 16;
	const uint32 ByteRate = static_cast<uint32>(SampleRate) * NumChannels * (BitsPerSample / 8);
	const uint16 BlockAlign = NumChannels * (BitsPerSample / 8);

	TArray<uint8> File;
	File.Reserve(44 + PcmBytes.Num());

	auto AppendU32 = [&File](uint32 V)
	{
		File.Add(V & 0xFF); File.Add((V >> 8) & 0xFF); File.Add((V >> 16) & 0xFF); File.Add((V >> 24) & 0xFF);
	};
	auto AppendU16 = [&File](uint16 V)
	{
		File.Add(V & 0xFF); File.Add((V >> 8) & 0xFF);
	};
	auto AppendTag = [&File](const ANSICHAR* Tag)
	{
		File.Append(reinterpret_cast<const uint8*>(Tag), 4);
	};

	AppendTag("RIFF"); AppendU32(36 + DataSize); AppendTag("WAVE");
	AppendTag("fmt "); AppendU32(16); AppendU16(1 /*PCM*/); AppendU16(NumChannels);
	AppendU32(static_cast<uint32>(SampleRate)); AppendU32(ByteRate); AppendU16(BlockAlign); AppendU16(BitsPerSample);
	AppendTag("data"); AppendU32(DataSize);
	File.Append(PcmBytes.GetData(), PcmBytes.Num());

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!FFileHelper::SaveArrayToFile(File, *Path))
	{
		if (OutError) { *OutError = FString::Printf(TEXT("failed to write %s"), *Path); }
		return false;
	}
	return true;
}

bool ReadMonoWavAsFloat32(const FString& Path, TArray<float>& OutSamples, int32& OutSampleRate, FString* OutError)
{
	OutSamples.Reset();
	OutSampleRate = 0;

	TArray<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *Path))
	{
		if (OutError) { *OutError = FString::Printf(TEXT("failed to read %s"), *Path); }
		return false;
	}
	if (File.Num() < 44 ||
		FMemory::Memcmp(File.GetData(), "RIFF", 4) != 0 ||
		FMemory::Memcmp(File.GetData() + 8, "WAVE", 4) != 0)
	{
		if (OutError) { *OutError = FString::Printf(TEXT("%s is not a RIFF/WAVE file"), *Path); }
		return false;
	}

	auto ReadU32 = [&File](int32 At) -> uint32
	{
		return static_cast<uint32>(File[At]) | (static_cast<uint32>(File[At + 1]) << 8) |
		       (static_cast<uint32>(File[At + 2]) << 16) | (static_cast<uint32>(File[At + 3]) << 24);
	};
	auto ReadU16 = [&File](int32 At) -> uint16
	{
		return static_cast<uint16>(File[At]) | (static_cast<uint16>(File[At + 1]) << 8);
	};

	// Walk RIFF chunks: capture fmt, then data. int64 cursor + monotonic
	// advance guard so a hostile/malformed chunk size can't overflow into
	// an out-of-bounds read.
	uint16 Format = 0, NumChannels = 0, BitsPerSample = 0;
	uint32 SampleRate = 0;
	int32 DataOffset = -1;
	uint32 DataSize = 0;
	int64 At = 12;
	while (At + 8 <= File.Num())
	{
		const int32 At32 = static_cast<int32>(At);
		const uint32 ChunkSize = ReadU32(At32 + 4);
		if (FMemory::Memcmp(File.GetData() + At32, "fmt ", 4) == 0 && ChunkSize >= 16 && At + 8 + 16 <= File.Num())
		{
			Format        = ReadU16(At32 + 8);
			NumChannels   = ReadU16(At32 + 10);
			SampleRate    = ReadU32(At32 + 12);
			BitsPerSample = ReadU16(At32 + 22);
		}
		else if (FMemory::Memcmp(File.GetData() + At32, "data", 4) == 0)
		{
			DataOffset = At32 + 8;
			DataSize = FMath::Min(ChunkSize, static_cast<uint32>(File.Num() - DataOffset));
		}
		At += 8 + static_cast<int64>(ChunkSize) + (ChunkSize & 1); // chunks are word-aligned
	}

	if (DataOffset < 0 || SampleRate == 0)
	{
		if (OutError) { *OutError = FString::Printf(TEXT("%s: missing fmt/data chunk"), *Path); }
		return false;
	}
	if (NumChannels != 1)
	{
		if (OutError) { *OutError = FString::Printf(TEXT("%s: expected mono, got %d channels (convert offline)"), *Path, NumChannels); }
		return false;
	}

	if (Format == 1 && BitsPerSample == 16)
	{
		return Int16PcmBytesToFloat32Mono(
			MakeArrayView(File.GetData() + DataOffset, static_cast<int32>(DataSize & ~1u)), OutSamples, OutError)
			&& (OutSampleRate = static_cast<int32>(SampleRate)) != 0;
	}
	if (Format == 3 && BitsPerSample == 32)
	{
		const int32 NumSamples = static_cast<int32>(DataSize / 4);
		OutSamples.AddUninitialized(NumSamples);
		FMemory::Memcpy(OutSamples.GetData(), File.GetData() + DataOffset, NumSamples * 4);
		OutSampleRate = static_cast<int32>(SampleRate);
		return true;
	}

	if (OutError)
	{
		*OutError = FString::Printf(TEXT("%s: unsupported format (fmt=%d bits=%d); expected PCM16 or float32 mono"),
			*Path, Format, BitsPerSample);
	}
	return false;
}

} // namespace InoSherpaPcm
