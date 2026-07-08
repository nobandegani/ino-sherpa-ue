// Copyright Inoland. All Rights Reserved.
//
// Console smoke tests for UInoSTT. All need a running PIE / game world.
//
//   Ino.Sherpa.STT.LoadTest <encoder.onnx> <decoder.onnx> <joiner.onnx> <tokens.txt>
//       Synchronous streaming-Zipformer model load.
//
//   Ino.Sherpa.STT.TranscribeTest <mono.wav>
//       Synchronous one-shot transcription of a whole WAV; logs the text.
//
//   Ino.Sherpa.STT.StreamTest <mono.wav>
//       StartStream + real-time-paced 100 ms pushes; logs every partial /
//       final / endpoint, then FinishStream + StopStream. Exercises the
//       full worker path including endpoint auto-segmentation.
//
// Dev model: run SherpaOnnx/scripts/get-dev-models.ps1, then point at
//   <plugin>/SherpaOnnx/models/sherpa-onnx-streaming-zipformer-en-2023-06-26/
//   (encoder/decoder/joiner *-epoch-99-avg-1-chunk-16-left-128.onnx + tokens.txt,
//    test_wavs/*.wav for input)

#include "SmokeTests/InoSherpaSttSmokeHelper.h"

#include "InoSherpa.h"
#include "InoSherpaPcm.h"
#include "STT/InoSTTSubsystem.h"
#include "SmokeTests/InoSherpaSmokeCommon.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"

void UInoSherpaSttSmokeHelper::HandlePartial(const FInoSTTResult& Result)
{
	++NumPartials;
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: partial #%d: '%s'"), NumPartials, *Result.Text);
}

void UInoSherpaSttSmokeHelper::HandleFinal(const FInoSTTResult& Result)
{
	++NumFinals;
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: FINAL #%d: '%s' (partials so far: %d)"),
		NumFinals, *Result.Text, NumPartials);
}

void UInoSherpaSttSmokeHelper::HandleEndpoint()
{
	++NumEndpoints;
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: endpoint #%d"), NumEndpoints);
}

namespace
{
TStrongObjectPtr<UInoSherpaSttSmokeHelper> GActiveSttHelper;
FTSTicker::FDelegateHandle GStreamTickerHandle;

UInoSTT* GetStt()
{
	UGameInstance* GameInstance = InoSherpaSmoke::FindGameInstance();
	if (GameInstance == nullptr)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: no PIE/game world running"));
		return nullptr;
	}
	return GameInstance->GetSubsystem<UInoSTT>();
}

void StopStreamTicker()
{
	if (GStreamTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GStreamTickerHandle);
		GStreamTickerHandle.Reset();
	}
}

void RunLoadTest(const TArray<FString>& Args)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("Usage: Ino.Sherpa.STT.LoadTest <encoder.onnx> <decoder.onnx> <joiner.onnx> <tokens.txt>"));
		return;
	}
	UInoSTT* Stt = GetStt();
	if (Stt == nullptr) { return; }

	FInoSTTModelConfig Config;
	Config.EncoderPath = Args[0];
	Config.DecoderPath = Args[1];
	Config.JoinerPath  = Args[2];
	Config.TokensPath  = Args[3];

	FString Error;
	if (Stt->LoadModel(Config, Error))
	{
		UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: LoadTest PASSED"));
	}
	else
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: LoadTest FAILED -- %s"), *Error);
	}
}

bool LoadWavArg(const TArray<FString>& Args, TArray<float>& OutSamples, int32& OutRate)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: expected a mono WAV path argument"));
		return false;
	}
	FString Error;
	if (!InoSherpaPcm::ReadMonoWavAsFloat32(Args[0], OutSamples, OutRate, &Error))
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: %s"), *Error);
		return false;
	}
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: read %s (%d samples @ %d Hz, %.2fs)"),
		*Args[0], OutSamples.Num(), OutRate, static_cast<float>(OutSamples.Num()) / OutRate);
	return true;
}

void RunTranscribeTest(const TArray<FString>& Args)
{
	UInoSTT* Stt = GetStt();
	if (Stt == nullptr) { return; }
	if (!Stt->IsModelLoaded())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: no model loaded -- run Ino.Sherpa.STT.LoadTest first"));
		return;
	}

	TArray<float> Samples;
	int32 Rate = 0;
	if (!LoadWavArg(Args, Samples, Rate)) { return; }

	const double Start = FPlatformTime::Seconds();
	const FInoSTTResult Result = Stt->TranscribeFloat(Samples, Rate);
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: TranscribeTest %s -- '%s' (%.2fs)"),
		Result.Text.IsEmpty() ? TEXT("produced NO text") : TEXT("PASSED"),
		*Result.Text, FPlatformTime::Seconds() - Start);
}

void RunStreamTest(const TArray<FString>& Args)
{
	UInoSTT* Stt = GetStt();
	if (Stt == nullptr) { return; }
	if (!Stt->IsModelLoaded())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: no model loaded -- run Ino.Sherpa.STT.LoadTest first"));
		return;
	}

	// Optional 2nd arg: number of passes over the same audio. Passes >= 2
	// exercise the FinishStream -> stream-recreation -> next-utterance path.
	const int32 NumPasses = Args.Num() >= 2 ? FMath::Max(1, FCString::Atoi(*Args[1])) : 1;

	TArray<float> Samples;
	int32 Rate = 0;
	if (!LoadWavArg(Args, Samples, Rate)) { return; }

	// Restart cleanly if a previous run left a session up.
	StopStreamTicker();
	if (Stt->IsStreaming())
	{
		Stt->StopStream();
	}

	GActiveSttHelper.Reset(NewObject<UInoSherpaSttSmokeHelper>());

	FInoSTTResultDelegate OnPartial;
	OnPartial.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandlePartial);
	FInoSTTFinalDelegate OnFinal;
	OnFinal.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandleFinal);
	FInoSTTEndpointDelegate OnEndpoint;
	OnEndpoint.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandleEndpoint);

	Stt->StartStream(OnPartial, OnFinal, OnEndpoint);
	if (!Stt->IsStreaming())
	{
		return;
	}

	// Push in real-time-paced ~100 ms chunks to mimic live capture.
	struct FPushState
	{
		TArray<float> Samples;
		int32 Rate = 0;
		int32 Cursor = 0;
		int32 PassesRemaining = 1;
	};
	TSharedPtr<FPushState> State = MakeShared<FPushState>();
	State->Samples = MoveTemp(Samples);
	State->Rate = Rate;
	State->PassesRemaining = NumPasses;

	TWeakObjectPtr<UInoSTT> WeakStt(Stt);
	GStreamTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[State, WeakStt](float /*DeltaTime*/) -> bool
		{
			UInoSTT* SttNow = WeakStt.Get();
			if (SttNow == nullptr || !SttNow->IsStreaming())
			{
				GStreamTickerHandle.Reset();
				return false; // world/session died; stop ticking
			}
			const int32 ChunkSamples = FMath::Max(1, State->Rate / 10);
			const int32 Remaining = State->Samples.Num() - State->Cursor;
			if (Remaining <= 0)
			{
				UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: input exhausted -> FinishStream"));
				SttNow->FinishStream();
				if (--State->PassesRemaining > 0)
				{
					// Next pass rides the SAME session: pushes queued after
					// Finish land on the recreated stream.
					UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: starting next pass (%d left)"), State->PassesRemaining);
					State->Cursor = 0;
					return true;
				}
				GStreamTickerHandle.Reset();
				return false;
			}
			const int32 Count = FMath::Min(ChunkSamples, Remaining);
			TArray<float> Chunk(State->Samples.GetData() + State->Cursor, Count);
			State->Cursor += Count;
			SttNow->PushAudioFloat(Chunk, State->Rate);
			return true; // keep ticking
		}), 0.1f);

	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: StreamTest started (%.2fs of audio, 100ms chunks, %d pass(es))"),
		static_cast<float>(State->Samples.Num()) / State->Rate, NumPasses);
}

void RunAbortTest(const TArray<FString>& Args)
{
	UInoSTT* Stt = GetStt();
	if (Stt == nullptr) { return; }
	if (!Stt->IsModelLoaded())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: no model loaded -- run Ino.Sherpa.STT.LoadTest first"));
		return;
	}

	TArray<float> Samples;
	int32 Rate = 0;
	if (!LoadWavArg(Args, Samples, Rate)) { return; }

	StopStreamTicker();
	if (Stt->IsStreaming())
	{
		Stt->StopStream();
	}

	GActiveSttHelper.Reset(NewObject<UInoSherpaSttSmokeHelper>());
	FInoSTTResultDelegate OnPartial;
	OnPartial.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandlePartial);
	FInoSTTFinalDelegate OnFinal;
	OnFinal.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandleFinal);
	FInoSTTEndpointDelegate OnEndpoint;
	OnEndpoint.BindDynamic(GActiveSttHelper.Get(), &UInoSherpaSttSmokeHelper::HandleEndpoint);

	// Session 1: shove ~1s of audio at the worker and tear the session
	// down immediately -- StopStream must join cleanly while the worker is
	// (very likely) mid-decode.
	Stt->StartStream(OnPartial, OnFinal, OnEndpoint);
	if (!Stt->IsStreaming()) { return; }
	const int32 OneSecond = FMath::Min(Samples.Num(), Rate);
	TArray<float> Burst(Samples.GetData(), OneSecond);
	Stt->PushAudioFloat(Burst, Rate);
	Stt->StopStream();
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: AbortTest -- mid-decode StopStream survived"));

	// Session 2 on the same model: full clip + Finish; a correct final
	// here proves the recognizer survived the aborted session.
	Stt->StartStream(OnPartial, OnFinal, OnEndpoint);
	if (!Stt->IsStreaming())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT.SmokeTest: AbortTest FAILED -- could not restart session"));
		return;
	}
	Stt->PushAudioFloat(Samples, Rate);
	Stt->FinishStream();
	UE_LOG(LogInoSherpa, Log, TEXT("STT.SmokeTest: AbortTest session 2 pushed %d samples; expecting a FINAL"), Samples.Num());
}

FAutoConsoleCommand GSttLoadTestCmd(
	TEXT("Ino.Sherpa.STT.LoadTest"),
	TEXT("Ino.Sherpa.STT.LoadTest <encoder.onnx> <decoder.onnx> <joiner.onnx> <tokens.txt> -- sync streaming-Zipformer load."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));

FAutoConsoleCommand GSttTranscribeTestCmd(
	TEXT("Ino.Sherpa.STT.TranscribeTest"),
	TEXT("Ino.Sherpa.STT.TranscribeTest <mono.wav> -- sync one-shot transcription, logs the text."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunTranscribeTest));

FAutoConsoleCommand GSttStreamTestCmd(
	TEXT("Ino.Sherpa.STT.StreamTest"),
	TEXT("Ino.Sherpa.STT.StreamTest <mono.wav> [passes] -- streaming session with paced pushes; logs partials/finals/endpoints. passes>=2 exercises session reuse after FinishStream."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamTest));

FAutoConsoleCommand GSttAbortTestCmd(
	TEXT("Ino.Sherpa.STT.AbortTest"),
	TEXT("Ino.Sherpa.STT.AbortTest <mono.wav> -- StopStream mid-decode, then a fresh session; expects a clean FINAL from session 2."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunAbortTest));
} // namespace
