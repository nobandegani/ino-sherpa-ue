// Copyright Inoland. All Rights Reserved.
//
// Console smoke tests for UInoTTS. All need a running PIE / game world.
//
//   Ino.Sherpa.TTS.LoadTest <model.onnx> <tokens.txt> <espeak-ng-data-dir>
//       Synchronous Piper model load; logs sample rate + speaker count.
//
//   Ino.Sherpa.TTS.SynthTest <text...>
//       Streaming async synthesis; logs each chunk, writes the full clip
//       to Saved/InoSherpa/tts.wav.
//
//   Ino.Sherpa.TTS.CancelTest [text...]
//       Kicks a long streaming synthesis and cancels on the same frame;
//       expects completion with bWasCancelled = true and partial audio.
//
// Dev models: run SherpaOnnx/scripts/get-dev-models.ps1, then e.g.
//   Ino.Sherpa.TTS.LoadTest <plugin>/SherpaOnnx/models/vits-piper-en_US-libritts_r-medium/en_US-libritts_r-medium.onnx
//                           <...>/tokens.txt <...>/espeak-ng-data

#include "SmokeTests/InoSherpaTtsSmokeHelper.h"

#include "InoSherpa.h"
#include "InoSherpaPcm.h"
#include "SmokeTests/InoSherpaSmokeCommon.h"
#include "TTS/InoTTSSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

void UInoSherpaTtsSmokeHelper::HandleChunk(const TArray<uint8>& AudioChunk, float Progress, bool bIsFinal)
{
	if (!bIsFinal)
	{
		++NumChunks;
		NumChunkBytes += AudioChunk.Num();
	}
	UE_LOG(LogInoSherpa, Log, TEXT("TTS.SmokeTest: chunk #%d, %d bytes, progress %.2f%s"),
		NumChunks, AudioChunk.Num(), Progress, bIsFinal ? TEXT(" (final)") : TEXT(""));
}

void UInoSherpaTtsSmokeHelper::HandleComplete(const FInoTTSResult& Result)
{
	UE_LOG(LogInoSherpa, Log,
		TEXT("TTS.SmokeTest: complete -- success=%d cancelled=%d rate=%d bytes=%d duration=%.2fs gen=%.2fs rtf=%.2f chunks=%d err='%s'"),
		Result.bSuccess ? 1 : 0, Result.bWasCancelled ? 1 : 0, Result.SampleRate, Result.AudioSamples.Num(),
		Result.DurationSeconds, Result.GenerationTimeSeconds, Result.RealTimeFactor, NumChunks, *Result.ErrorMessage);

	if (bExpectCancelled && !Result.bWasCancelled)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("TTS.SmokeTest: CancelTest expected bWasCancelled=true (synthesis may have finished before the cancel -- use longer text)"));
	}
	if (bExpectCancelled && Result.bWasCancelled)
	{
		UE_LOG(LogInoSherpa, Log, TEXT("TTS.SmokeTest: CancelTest PASSED (partial audio: %d bytes)"), Result.AudioSamples.Num());
	}

	if (Result.bSuccess && !WavOutPath.IsEmpty() && Result.AudioSamples.Num() > 0)
	{
		FString Error;
		if (InoSherpaPcm::WriteInt16PcmBytesAsWav(WavOutPath, Result.AudioSamples, Result.SampleRate, &Error))
		{
			UE_LOG(LogInoSherpa, Log, TEXT("TTS.SmokeTest: wrote %s"), *WavOutPath);
		}
		else
		{
			UE_LOG(LogInoSherpa, Error, TEXT("TTS.SmokeTest: WAV write failed: %s"), *Error);
		}
	}
}

namespace
{
TStrongObjectPtr<UInoSherpaTtsSmokeHelper> GActiveTtsHelper;

UInoTTS* GetTts()
{
	UGameInstance* GameInstance = InoSherpaSmoke::FindGameInstance();
	if (GameInstance == nullptr)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("TTS.SmokeTest: no PIE/game world running"));
		return nullptr;
	}
	return GameInstance->GetSubsystem<UInoTTS>();
}

void RunLoadTest(const TArray<FString>& Args)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("Usage: Ino.Sherpa.TTS.LoadTest <model.onnx> <tokens.txt> <espeak-ng-data-dir>"));
		return;
	}
	UInoTTS* Tts = GetTts();
	if (Tts == nullptr) { return; }

	FInoTTSModelConfig Config;
	Config.ModelType = EInoTTSModelType::PiperVits;
	Config.Piper.ModelPath  = Args[0];
	Config.Piper.TokensPath = Args[1];
	Config.Piper.DataDir    = Args[2];

	FString Error;
	if (Tts->LoadModel(Config, Error))
	{
		UE_LOG(LogInoSherpa, Log, TEXT("TTS.SmokeTest: LoadTest PASSED -- rate=%d speakers=%d"),
			Tts->GetSampleRate(), Tts->GetNumSpeakers());
	}
	else
	{
		UE_LOG(LogInoSherpa, Error, TEXT("TTS.SmokeTest: LoadTest FAILED -- %s"), *Error);
	}
}

void StartStreamingSynth(const FString& Text, bool bCancelImmediately)
{
	UInoTTS* Tts = GetTts();
	if (Tts == nullptr) { return; }
	if (!Tts->IsModelLoaded())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("TTS.SmokeTest: no model loaded -- run Ino.Sherpa.TTS.LoadTest first"));
		return;
	}

	GActiveTtsHelper.Reset(NewObject<UInoSherpaTtsSmokeHelper>());
	GActiveTtsHelper->WavOutPath = bCancelImmediately
		? FString()
		: FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InoSherpa"), TEXT("tts.wav"));
	GActiveTtsHelper->bExpectCancelled = bCancelImmediately;

	FInoTTSAudioChunkDelegate OnChunk;
	OnChunk.BindDynamic(GActiveTtsHelper.Get(), &UInoSherpaTtsSmokeHelper::HandleChunk);
	FInoTTSCompleteDelegate OnComplete;
	OnComplete.BindDynamic(GActiveTtsHelper.Get(), &UInoSherpaTtsSmokeHelper::HandleComplete);

	Tts->SynthesizeStreamAsync(Text, FInoTTSOptions(), OnChunk, OnComplete);
	if (bCancelImmediately)
	{
		Tts->CancelSynthesis();
	}
}

void RunSynthTest(const TArray<FString>& Args)
{
	const FString Text = Args.Num() > 0
		? FString::Join(Args, TEXT(" "))
		: TEXT("Hello from Ino Sherpa. This is the text to speech smoke test.");
	StartStreamingSynth(Text, /*bCancelImmediately=*/false);
}

void RunCancelTest(const TArray<FString>& Args)
{
	// Long enough that generation is guaranteed to still be running when
	// the cancel lands.
	FString Text = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : FString();
	if (Text.IsEmpty())
	{
		for (int32 i = 0; i < 12; ++i)
		{
			Text += TEXT("This sentence exists only to keep the synthesizer busy for a while. ");
		}
	}
	StartStreamingSynth(Text, /*bCancelImmediately=*/true);
}

FAutoConsoleCommand GTtsLoadTestCmd(
	TEXT("Ino.Sherpa.TTS.LoadTest"),
	TEXT("Ino.Sherpa.TTS.LoadTest <model.onnx> <tokens.txt> <espeak-ng-data-dir> -- sync Piper load, logs rate/speakers."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));

FAutoConsoleCommand GTtsSynthTestCmd(
	TEXT("Ino.Sherpa.TTS.SynthTest"),
	TEXT("Ino.Sherpa.TTS.SynthTest <text...> -- streaming synth, writes Saved/InoSherpa/tts.wav."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunSynthTest));

FAutoConsoleCommand GTtsCancelTestCmd(
	TEXT("Ino.Sherpa.TTS.CancelTest"),
	TEXT("Ino.Sherpa.TTS.CancelTest [text...] -- kicks a long synth and cancels immediately; expects bWasCancelled."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunCancelTest));
} // namespace
