// Copyright Inoland. All Rights Reserved.

#include "TTS/InoTTSSubsystem.h"

#include "InoSherpa.h"
#include "TTS/InoTtsEngine.h"

#include "Async/Async.h"

void UInoTTS::Deinitialize()
{
	// Teardown ordering: signal the cancel flag first so an in-flight
	// generate aborts at its next callback tick, then drop our engine ref.
	// The worker holds its own TSharedPtr copy, so the sherpa handle stays
	// alive until that worker returns; its game-thread marshal then sees a
	// dead weak-this and fires nothing.
	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true, std::memory_order_release);
	}
	CurrentCancelFlag.Reset();
	Engine.Reset();
	CachedSampleRate = 0;
	CachedNumSpeakers = 0;

	Super::Deinitialize();
}

void UInoTTS::LoadModelAsync(const FInoTTSModelConfig& Config, const FInoTTSLoadedDelegate& OnLoaded)
{
	check(IsInGameThread());

	if (bIsLoading)
	{
		OnLoaded.ExecuteIfBound(false, TEXT("a model load is already in flight"));
		return;
	}
	if (Engine.IsValid())
	{
		OnLoaded.ExecuteIfBound(false, TEXT("a model is already loaded -- UnloadModel first"));
		return;
	}

	bIsLoading = true;
	TWeakObjectPtr<UInoTTS> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, Config, OnLoaded]()
	{
		FString Error;
		TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> NewEngine = FInoTtsEngine::Create(Config, Error);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, NewEngine, Error, OnLoaded]()
		{
			UInoTTS* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return; // subsystem died mid-load; NewEngine's last ref frees the handle
			}
			Self->bIsLoading = false;
			if (NewEngine.IsValid())
			{
				Self->Engine = NewEngine;
				Self->CachedSampleRate = NewEngine->GetSampleRate();
				Self->CachedNumSpeakers = NewEngine->GetNumSpeakers();
				OnLoaded.ExecuteIfBound(true, FString());
			}
			else
			{
				OnLoaded.ExecuteIfBound(false, Error);
			}
		});
	});
}

bool UInoTTS::LoadModel(const FInoTTSModelConfig& Config, FString& OutError)
{
	check(IsInGameThread());

	if (bIsLoading)
	{
		OutError = TEXT("a model load is already in flight");
		return false;
	}
	if (Engine.IsValid())
	{
		OutError = TEXT("a model is already loaded -- UnloadModel first");
		return false;
	}

	TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> NewEngine = FInoTtsEngine::Create(Config, OutError);
	if (!NewEngine.IsValid())
	{
		return false;
	}
	Engine = NewEngine;
	CachedSampleRate = NewEngine->GetSampleRate();
	CachedNumSpeakers = NewEngine->GetNumSpeakers();
	return true;
}

void UInoTTS::UnloadModel()
{
	check(IsInGameThread());

	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true, std::memory_order_release);
	}
	Engine.Reset();
	CachedSampleRate = 0;
	CachedNumSpeakers = 0;
}

bool UInoTTS::IsModelLoaded() const
{
	return Engine.IsValid();
}

void UInoTTS::SynthesizeAsync(const FString& Text, const FInoTTSOptions& Options,
	const FInoTTSCompleteDelegate& OnComplete)
{
	StartSynthesisInternal(Text, Options, FInoTTSAudioChunkDelegate(), /*bStreaming=*/false, OnComplete);
}

void UInoTTS::SynthesizeStreamAsync(const FString& Text, const FInoTTSOptions& Options,
	const FInoTTSAudioChunkDelegate& OnAudioChunk,
	const FInoTTSCompleteDelegate& OnComplete)
{
	StartSynthesisInternal(Text, Options, OnAudioChunk, /*bStreaming=*/true, OnComplete);
}

void UInoTTS::StartSynthesisInternal(const FString& Text, const FInoTTSOptions& Options,
	FInoTTSAudioChunkDelegate OnAudioChunk, bool bStreaming,
	FInoTTSCompleteDelegate OnComplete)
{
	check(IsInGameThread());

	auto FailNow = [&OnComplete](const TCHAR* Why)
	{
		FInoTTSResult Failed;
		Failed.ErrorMessage = Why;
		OnComplete.ExecuteIfBound(Failed);
	};

	if (!Engine.IsValid())
	{
		FailNow(TEXT("no model loaded"));
		return;
	}
	if (bSynthInFlight)
	{
		FailNow(TEXT("a synthesis is already in flight"));
		return;
	}

	bSynthInFlight = true;
	CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	TWeakObjectPtr<UInoTTS> WeakThis(this);
	TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> EngineCopy = Engine;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelCopy = CurrentCancelFlag;

	Async(EAsyncExecution::ThreadPool,
		[WeakThis, EngineCopy, CancelCopy, Text, Options, OnAudioChunk, bStreaming, OnComplete]()
	{
		// Bridge each sherpa chunk (worker thread) to the game thread.
		// Weak-this is only dereferenced inside the game-thread lambda.
		FInoTtsEngine::FChunkFn ChunkBridge;
		if (bStreaming)
		{
			ChunkBridge = [WeakThis, OnAudioChunk](TArray<uint8>&& Bytes, float Progress)
			{
				AsyncTask(ENamedThreads::GameThread,
					[WeakThis, OnAudioChunk, Bytes = MoveTemp(Bytes), Progress]()
				{
					if (WeakThis.IsValid())
					{
						OnAudioChunk.ExecuteIfBound(Bytes, Progress, /*bIsFinal=*/false);
					}
				});
			};
		}

		FInoTTSResult Result = EngineCopy->Generate(Text, Options, CancelCopy, ChunkBridge);

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, OnAudioChunk, bStreaming, OnComplete, Result = MoveTemp(Result)]() mutable
		{
			if (UInoTTS* Self = WeakThis.Get())
			{
				Self->bSynthInFlight = false;
				Self->CurrentCancelFlag.Reset();
			}
			else
			{
				return; // PIE stop / teardown: no delegates into a dead world
			}
			if (bStreaming)
			{
				// Empty terminal chunk so stream consumers can finalize.
				OnAudioChunk.ExecuteIfBound(TArray<uint8>(), 1.0f, /*bIsFinal=*/true);
			}
			OnComplete.ExecuteIfBound(Result);
		});
	});
}

FInoTTSResult UInoTTS::Synthesize(const FString& Text, const FInoTTSOptions& Options)
{
	check(IsInGameThread());

	FInoTTSResult Result;
	if (!Engine.IsValid())
	{
		Result.ErrorMessage = TEXT("no model loaded");
		return Result;
	}
	if (bSynthInFlight)
	{
		Result.ErrorMessage = TEXT("an async synthesis is in flight");
		return Result;
	}
	// Blocking on the game thread by design (tooling / short lines); no
	// cancel flag -- nothing else can run to request cancellation anyway.
	return Engine->Generate(Text, Options, nullptr, FInoTtsEngine::FChunkFn());
}

void UInoTTS::CancelSynthesis()
{
	check(IsInGameThread());

	if (CurrentCancelFlag.IsValid())
	{
		CurrentCancelFlag->store(true, std::memory_order_release);
		UE_LOG(LogInoSherpa, Log, TEXT("TTS: cancel requested"));
	}
}
