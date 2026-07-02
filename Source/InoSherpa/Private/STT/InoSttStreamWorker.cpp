// Copyright Inoland. All Rights Reserved.

#include "STT/InoSttStreamWorker.h"

#include "InoSherpa.h"
#include "STT/InoSTTSubsystem.h"
#include "STT/InoSttRecognizer.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

#include "sherpa-onnx/c-api/c-api.h"

FInoSttStreamWorker::FInoSttStreamWorker(const TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe>& InRecognizer,
	const TWeakObjectPtr<UInoSTT>& InWeakOwner)
	: Recognizer(InRecognizer)
	, WeakOwner(InWeakOwner)
{
	check(Recognizer.IsValid());

	Stream = SherpaOnnxCreateOnlineStream(Recognizer->GetHandle());
	if (Stream == nullptr)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT: SherpaOnnxCreateOnlineStream failed"));
		return;
	}

	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);
	Thread.Reset(FRunnableThread::Create(this, TEXT("InoSttStreamWorker"), 0, TPri_Normal));
}

FInoSttStreamWorker::~FInoSttStreamWorker()
{
	// Join FIRST -- guarantees no in-flight sherpa call -- THEN free the
	// stream. Runs on the game thread (StopStream / UnloadModel /
	// Deinitialize).
	if (Thread.IsValid())
	{
		Stop();
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
	if (Stream != nullptr)
	{
		SherpaOnnxDestroyOnlineStream(Stream);
		Stream = nullptr;
	}
}

void FInoSttStreamWorker::EnqueuePush(TArray<float>&& Samples, int32 SampleRate)
{
	FSttCommand Cmd;
	Cmd.Type = FSttCommand::EType::PushAudio;
	Cmd.Samples = MoveTemp(Samples);
	Cmd.SampleRate = SampleRate;
	CommandQueue.Enqueue(MoveTemp(Cmd));
	WakeEvent->Trigger();
}

void FInoSttStreamWorker::EnqueueFinish()
{
	FSttCommand Cmd;
	Cmd.Type = FSttCommand::EType::Finish;
	CommandQueue.Enqueue(MoveTemp(Cmd));
	WakeEvent->Trigger();
}

void FInoSttStreamWorker::EnqueueReset()
{
	FSttCommand Cmd;
	Cmd.Type = FSttCommand::EType::Reset;
	CommandQueue.Enqueue(MoveTemp(Cmd));
	WakeEvent->Trigger();
}

void FInoSttStreamWorker::Stop()
{
	bStopRequested = true;
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}

uint32 FInoSttStreamWorker::Run()
{
	const SherpaOnnxOnlineRecognizer* Rec = Recognizer->GetHandle();

	while (!bStopRequested)
	{
		WakeEvent->Wait();
		if (bStopRequested)
		{
			break;
		}

		FSttCommand Cmd;
		while (!bStopRequested && CommandQueue.Dequeue(Cmd))
		{
			switch (Cmd.Type)
			{
			case FSttCommand::EType::PushAudio:
				if (Cmd.Samples.Num() > 0)
				{
					SherpaOnnxOnlineStreamAcceptWaveform(Stream, Cmd.SampleRate, Cmd.Samples.GetData(), Cmd.Samples.Num());
					PumpAndDispatch();
				}
				break;

			case FSttCommand::EType::Finish:
			{
				// Tail padding: without trailing right-context the last
				// word of the utterance gets truncated (upstream examples
				// pad the same way).
				const int32 FeatRate = Recognizer->GetFeatSampleRate();
				TArray<float> TailSilence;
				TailSilence.AddZeroed(FMath::Max(1, (FeatRate * 6) / 10)); // 0.6s
				SherpaOnnxOnlineStreamAcceptWaveform(Stream, FeatRate, TailSilence.GetData(), TailSilence.Num());

				SherpaOnnxOnlineStreamInputFinished(Stream);
				while (SherpaOnnxIsOnlineStreamReady(Rec, Stream))
				{
					SherpaOnnxDecodeOnlineStream(Rec, Stream);
				}
				const FString Text = ReadResultText();
				if (!Text.IsEmpty())
				{
					DispatchFinal(Text);
				}
				// A finished stream accepts no more audio; start fresh so
				// the session survives FinishStream -> next utterance.
				RecreateStream();
				Rec = Recognizer->GetHandle();
				break;
			}

			case FSttCommand::EType::Reset:
			{
				const FString Text = ReadResultText();
				if (!Text.IsEmpty())
				{
					DispatchFinal(Text);
				}
				SherpaOnnxOnlineStreamReset(Rec, Stream);
				LastDispatchedText.Empty();
				break;
			}
			}
		}
	}
	return 0;
}

void FInoSttStreamWorker::PumpAndDispatch()
{
	const SherpaOnnxOnlineRecognizer* Rec = Recognizer->GetHandle();

	while (SherpaOnnxIsOnlineStreamReady(Rec, Stream))
	{
		SherpaOnnxDecodeOnlineStream(Rec, Stream);
	}

	const FString Text = ReadResultText();
	if (!Text.IsEmpty() && Text != LastDispatchedText)
	{
		DispatchPartial(Text);
		LastDispatchedText = Text;
	}

	if (SherpaOnnxOnlineStreamIsEndpoint(Rec, Stream) != 0)
	{
		// Only report an utterance when the endpoint actually segmented
		// speech; silence-only endpoints (rule 1) reset quietly, else
		// long pauses would spam final/endpoint events.
		if (!Text.IsEmpty())
		{
			DispatchFinal(Text);
			DispatchEndpoint();
		}
		SherpaOnnxOnlineStreamReset(Rec, Stream);
		LastDispatchedText.Empty();
	}
}

FString FInoSttStreamWorker::ReadResultText() const
{
	FString Text;
	if (const SherpaOnnxOnlineRecognizerResult* Result = SherpaOnnxGetOnlineStreamResult(Recognizer->GetHandle(), Stream))
	{
		Text = UTF8_TO_TCHAR(Result->text);
		SherpaOnnxDestroyOnlineRecognizerResult(Result);
	}
	return Text;
}

void FInoSttStreamWorker::RecreateStream()
{
	SherpaOnnxDestroyOnlineStream(Stream);
	Stream = SherpaOnnxCreateOnlineStream(Recognizer->GetHandle());
	LastDispatchedText.Empty();
	if (Stream == nullptr)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT: stream recreation after Finish failed"));
		bStopRequested = true;
	}
}

void FInoSttStreamWorker::DispatchPartial(const FString& Text) const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	AsyncTask(ENamedThreads::GameThread, [Weak, Text]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyPartialFromWorker(Text);
		}
	});
}

void FInoSttStreamWorker::DispatchFinal(const FString& Text) const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	AsyncTask(ENamedThreads::GameThread, [Weak, Text]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyFinalFromWorker(Text);
		}
	});
}

void FInoSttStreamWorker::DispatchEndpoint() const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	AsyncTask(ENamedThreads::GameThread, [Weak]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyEndpointFromWorker();
		}
	});
}
