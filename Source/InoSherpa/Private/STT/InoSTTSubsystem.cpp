// Copyright Inoland. All Rights Reserved.

#include "STT/InoSTTSubsystem.h"

#include "InoSherpa.h"
#include "InoSherpaPcm.h"
#include "STT/InoSttOfflineRecognizer.h"
#include "STT/InoSttRecognizer.h"
#include "STT/InoSttStreamWorker.h"

#include "Async/Async.h"

void UInoSTT::Deinitialize()
{
	// Worker destructor joins its thread BEFORE the stream dies; only then
	// may the recognizer go (the stream references it). In-flight offline
	// transcribes hold their own recognizer copy and die on the weak-this.
	StreamWorker.Reset();
	Recognizer.Reset();
	OfflineRecognizer.Reset();
	OnPartialDelegate.Unbind();
	OnFinalDelegate.Unbind();
	OnEndpointDelegate.Unbind();

	Super::Deinitialize();
}

void UInoSTT::LoadModelAsync(const FInoSTTModelConfig& Config, const FInoSTTLoadedDelegate& OnLoaded)
{
	check(IsInGameThread());

	if (bIsLoading)
	{
		OnLoaded.ExecuteIfBound(false, TEXT("a model load is already in flight"));
		return;
	}
	if (Recognizer.IsValid())
	{
		OnLoaded.ExecuteIfBound(false, TEXT("a model is already loaded -- UnloadModel first"));
		return;
	}

	bIsLoading = true;
	TWeakObjectPtr<UInoSTT> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, Config, OnLoaded]()
	{
		FString Error;
		TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> NewRecognizer = FInoSttRecognizer::Create(Config, Error);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, NewRecognizer, Error, OnLoaded]()
		{
			UInoSTT* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			Self->bIsLoading = false;
			if (NewRecognizer.IsValid())
			{
				Self->Recognizer = NewRecognizer;
				OnLoaded.ExecuteIfBound(true, FString());
			}
			else
			{
				OnLoaded.ExecuteIfBound(false, Error);
			}
		});
	});
}

bool UInoSTT::LoadModel(const FInoSTTModelConfig& Config, FString& OutError)
{
	check(IsInGameThread());

	if (bIsLoading)
	{
		OutError = TEXT("a model load is already in flight");
		return false;
	}
	if (Recognizer.IsValid())
	{
		OutError = TEXT("a model is already loaded -- UnloadModel first");
		return false;
	}

	Recognizer = FInoSttRecognizer::Create(Config, OutError);
	return Recognizer.IsValid();
}

void UInoSTT::UnloadModel()
{
	check(IsInGameThread());

	StreamWorker.Reset(); // joins the worker thread first
	Recognizer.Reset();
}

bool UInoSTT::IsModelLoaded() const
{
	return Recognizer.IsValid();
}

void UInoSTT::StartStream(const FInoSTTResultDelegate& OnPartial,
	const FInoSTTFinalDelegate& OnFinal,
	const FInoSTTEndpointDelegate& OnEndpoint)
{
	check(IsInGameThread());

	if (!ensureMsgf(Recognizer.IsValid(), TEXT("InoSTT: StartStream without a loaded model")))
	{
		return;
	}
	if (StreamWorker.IsValid())
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: StartStream while already streaming -- call StopStream first; ignoring"));
		return;
	}
	// Single-decode-at-a-time invariant also holds against one-shots: an
	// in-flight TranscribeAsync is decoding its own stream on the pool.
	if (bTranscribeInFlight)
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: StartStream while a TranscribeAsync is in flight -- retry after it completes; ignoring"));
		return;
	}

	OnPartialDelegate = OnPartial;
	OnFinalDelegate = OnFinal;
	OnEndpointDelegate = OnEndpoint;
	++StreamSessionSerial; // invalidates any still-queued old-session events

	TSharedPtr<FInoSttStreamWorker> NewWorker =
		MakeShared<FInoSttStreamWorker>(Recognizer, TWeakObjectPtr<UInoSTT>(this), StreamSessionSerial);
	if (!NewWorker->IsHealthy())
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT: failed to start streaming session"));
		return;
	}
	StreamWorker = NewWorker;
	UE_LOG(LogInoSherpa, Log, TEXT("STT: streaming session started"));
}

void UInoSTT::PushAudioInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate)
{
	check(IsInGameThread());

	if (!ensureMsgf(StreamWorker.IsValid(), TEXT("InoSTT: PushAudioInt16 without StartStream")))
	{
		return;
	}
	TArray<float> Samples;
	FString Error;
	if (!InoSherpaPcm::Int16PcmBytesToFloat32Mono(Int16PcmLE, Samples, &Error))
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: PushAudioInt16 rejected: %s"), *Error);
		return;
	}
	StreamWorker->EnqueuePush(MoveTemp(Samples), SampleRate);
}

void UInoSTT::PushAudioFloat(const TArray<float>& Samples, int32 SampleRate)
{
	check(IsInGameThread());

	if (!ensureMsgf(StreamWorker.IsValid(), TEXT("InoSTT: PushAudioFloat without StartStream")))
	{
		return;
	}
	TArray<float> Copy = Samples;
	StreamWorker->EnqueuePush(MoveTemp(Copy), SampleRate);
}

void UInoSTT::FinishStream()
{
	check(IsInGameThread());

	if (!ensureMsgf(StreamWorker.IsValid(), TEXT("InoSTT: FinishStream without StartStream")))
	{
		return;
	}
	StreamWorker->EnqueueFinish();
}

void UInoSTT::ResetStream()
{
	check(IsInGameThread());

	if (!ensureMsgf(StreamWorker.IsValid(), TEXT("InoSTT: ResetStream without StartStream")))
	{
		return;
	}
	StreamWorker->EnqueueReset();
}

void UInoSTT::StopStream()
{
	check(IsInGameThread());

	if (StreamWorker.IsValid())
	{
		StreamWorker.Reset(); // dtor joins the thread, then frees the stream
		UE_LOG(LogInoSherpa, Log, TEXT("STT: streaming session stopped"));
	}
	++StreamSessionSerial; // drop this session's still-queued events
	OnPartialDelegate.Unbind();
	OnFinalDelegate.Unbind();
	OnEndpointDelegate.Unbind();
}

bool UInoSTT::IsStreaming() const
{
	return StreamWorker.IsValid();
}

FInoSTTResult UInoSTT::TranscribeInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate)
{
	check(IsInGameThread());

	FInoSTTResult Result;
	TArray<float> Samples;
	FString Error;
	if (!InoSherpaPcm::Int16PcmBytesToFloat32Mono(Int16PcmLE, Samples, &Error))
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeInt16 rejected: %s"), *Error);
		return Result;
	}
	return TranscribeFloat(Samples, SampleRate);
}

FInoSTTResult UInoSTT::TranscribeFloat(const TArray<float>& Samples, int32 SampleRate)
{
	check(IsInGameThread());

	FInoSTTResult Result;
	Result.bIsFinal = true;

	if (!Recognizer.IsValid())
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: Transcribe without a loaded model"));
		return Result;
	}
	// Single-decode-at-a-time invariant: sherpa gives no thread-safety
	// guarantees for concurrently decoding two streams of one recognizer.
	if (StreamWorker.IsValid() || bTranscribeInFlight)
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: Transcribe refused -- a streaming session or transcribe is active"));
		return Result;
	}

	Result.Text = Recognizer->TranscribeOnce(Samples, SampleRate);
	return Result;
}

void UInoSTT::TranscribeAsync(const TArray<float>& Samples, int32 SampleRate,
	const FInoSTTFinalDelegate& OnComplete)
{
	check(IsInGameThread());

	auto FailNow = [&OnComplete](const TCHAR* Why)
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeAsync refused -- %s"), Why);
		FInoSTTResult Failed;
		Failed.bIsFinal = true;
		OnComplete.ExecuteIfBound(Failed);
	};

	if (!Recognizer.IsValid())
	{
		FailNow(TEXT("no model loaded"));
		return;
	}
	if (StreamWorker.IsValid() || bTranscribeInFlight)
	{
		FailNow(TEXT("a streaming session or transcribe is active"));
		return;
	}

	bTranscribeInFlight = true;
	TWeakObjectPtr<UInoSTT> WeakThis(this);
	TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> RecognizerCopy = Recognizer;
	TArray<float> SamplesCopy = Samples;

	Async(EAsyncExecution::ThreadPool, [WeakThis, RecognizerCopy, SamplesCopy = MoveTemp(SamplesCopy), SampleRate, OnComplete]()
	{
		FInoSTTResult Result;
		Result.bIsFinal = true;
		Result.Text = RecognizerCopy->TranscribeOnce(SamplesCopy, SampleRate);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, OnComplete, Result = MoveTemp(Result)]()
		{
			if (UInoSTT* Self = WeakThis.Get())
			{
				Self->bTranscribeInFlight = false;
				OnComplete.ExecuteIfBound(Result);
			}
		});
	});
}

void UInoSTT::LoadOfflineModelAsync(const FInoSTTOfflineModelConfig& Config, const FInoSTTLoadedDelegate& OnLoaded)
{
	check(IsInGameThread());

	if (bOfflineLoading)
	{
		OnLoaded.ExecuteIfBound(false, TEXT("an offline model load is already in flight"));
		return;
	}
	if (OfflineRecognizer.IsValid())
	{
		OnLoaded.ExecuteIfBound(false, TEXT("an offline model is already loaded -- UnloadOfflineModel first"));
		return;
	}

	bOfflineLoading = true;
	TWeakObjectPtr<UInoSTT> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, Config, OnLoaded]()
	{
		FString Error;
		TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> NewRecognizer = FInoSttOfflineRecognizer::Create(Config, Error);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, NewRecognizer, Error, OnLoaded]()
		{
			UInoSTT* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			Self->bOfflineLoading = false;
			if (NewRecognizer.IsValid())
			{
				Self->OfflineRecognizer = NewRecognizer;
				OnLoaded.ExecuteIfBound(true, FString());
			}
			else
			{
				OnLoaded.ExecuteIfBound(false, Error);
			}
		});
	});
}

bool UInoSTT::LoadOfflineModel(const FInoSTTOfflineModelConfig& Config, FString& OutError)
{
	check(IsInGameThread());

	if (bOfflineLoading)
	{
		OutError = TEXT("an offline model load is already in flight");
		return false;
	}
	if (OfflineRecognizer.IsValid())
	{
		OutError = TEXT("an offline model is already loaded -- UnloadOfflineModel first");
		return false;
	}

	OfflineRecognizer = FInoSttOfflineRecognizer::Create(Config, OutError);
	return OfflineRecognizer.IsValid();
}

void UInoSTT::UnloadOfflineModel()
{
	check(IsInGameThread());

	OfflineRecognizer.Reset(); // in-flight async transcribes hold their own copy
}

bool UInoSTT::IsOfflineModelLoaded() const
{
	return OfflineRecognizer.IsValid();
}

FInoSTTResult UInoSTT::TranscribeOfflineInt16(const TArray<uint8>& Int16PcmLE, int32 SampleRate)
{
	check(IsInGameThread());

	FInoSTTResult Result;
	Result.bIsFinal = true;
	TArray<float> Samples;
	FString Error;
	if (!InoSherpaPcm::Int16PcmBytesToFloat32Mono(Int16PcmLE, Samples, &Error))
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeOfflineInt16 rejected: %s"), *Error);
		return Result;
	}
	return TranscribeOfflineFloat(Samples, SampleRate);
}

FInoSTTResult UInoSTT::TranscribeOfflineFloat(const TArray<float>& Samples, int32 SampleRate)
{
	check(IsInGameThread());

	FInoSTTResult Result;
	Result.bIsFinal = true;

	if (!OfflineRecognizer.IsValid())
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeOffline without a loaded offline model"));
		return Result;
	}
	if (bOfflineTranscribeInFlight)
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeOffline refused -- an offline transcribe is already in flight"));
		return Result;
	}

	Result.Text = OfflineRecognizer->Transcribe(Samples, SampleRate);
	return Result;
}

void UInoSTT::TranscribeOfflineAsync(const TArray<float>& Samples, int32 SampleRate,
	const FInoSTTFinalDelegate& OnComplete)
{
	check(IsInGameThread());

	auto FailNow = [&OnComplete](const TCHAR* Why)
	{
		UE_LOG(LogInoSherpa, Warning, TEXT("STT: TranscribeOfflineAsync refused -- %s"), Why);
		FInoSTTResult Failed;
		Failed.bIsFinal = true;
		OnComplete.ExecuteIfBound(Failed);
	};

	if (!OfflineRecognizer.IsValid())
	{
		FailNow(TEXT("no offline model loaded"));
		return;
	}
	if (bOfflineTranscribeInFlight)
	{
		FailNow(TEXT("an offline transcribe is already in flight"));
		return;
	}

	bOfflineTranscribeInFlight = true;
	TWeakObjectPtr<UInoSTT> WeakThis(this);
	TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> RecognizerCopy = OfflineRecognizer;
	TArray<float> SamplesCopy = Samples;

	Async(EAsyncExecution::ThreadPool, [WeakThis, RecognizerCopy, SamplesCopy = MoveTemp(SamplesCopy), SampleRate, OnComplete]()
	{
		FInoSTTResult Result;
		Result.bIsFinal = true;
		Result.Text = RecognizerCopy->Transcribe(SamplesCopy, SampleRate);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, OnComplete, Result = MoveTemp(Result)]()
		{
			if (UInoSTT* Self = WeakThis.Get())
			{
				Self->bOfflineTranscribeInFlight = false;
				OnComplete.ExecuteIfBound(Result);
			}
		});
	});
}

void UInoSTT::NotifyPartialFromWorker(const FString& Text, int32 SessionSerial)
{
	if (SessionSerial != StreamSessionSerial)
	{
		return; // stale event from a previous session
	}
	FInoSTTResult Result;
	Result.Text = Text;
	Result.bIsFinal = false;
	OnPartialDelegate.ExecuteIfBound(Result);
}

void UInoSTT::NotifyFinalFromWorker(const FString& Text, int32 SessionSerial)
{
	if (SessionSerial != StreamSessionSerial)
	{
		return;
	}
	FInoSTTResult Result;
	Result.Text = Text;
	Result.bIsFinal = true;
	OnFinalDelegate.ExecuteIfBound(Result);
}

void UInoSTT::NotifyEndpointFromWorker(int32 SessionSerial)
{
	if (SessionSerial != StreamSessionSerial)
	{
		return;
	}
	OnEndpointDelegate.ExecuteIfBound();
}

void UInoSTT::NotifyWorkerDiedFromWorker(int32 SessionSerial)
{
	if (SessionSerial != StreamSessionSerial || !StreamWorker.IsValid())
	{
		return;
	}
	UE_LOG(LogInoSherpa, Error, TEXT("STT: streaming worker died (stream recreation failed) -- dropping the session"));
	StopStream(); // joins the already-exited thread; unbinds; bumps serial
}
