// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "STT/InoSttStreamWorker.h"

#include "InoSherpa.h"
#include "STT/InoSTTSubsystem.h"
#include "STT/InoSttRecognizer.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

#if WITH_INO_SHERPA

#include "sherpa-onnx/c-api/c-api.h"

FInoSttStreamWorker::FInoSttStreamWorker(const TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe>& InRecognizer,
	const TWeakObjectPtr<UInoSTT>& InWeakOwner, int32 InSessionSerial)
	: Recognizer(InRecognizer)
	, WeakOwner(InWeakOwner)
	, SessionSerial(InSessionSerial)
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
					// sherpa keys the stream's resampler to the FIRST rate
					// it sees and process-EXITs on any later mismatch --
					// drop offending chunks instead of dying.
					if (EstablishedRate == 0)
					{
						EstablishedRate = Cmd.SampleRate;
					}
					else if (Cmd.SampleRate != EstablishedRate)
					{
						UE_LOG(LogInoSherpa, Error,
							TEXT("STT: dropped %d samples pushed at %d Hz -- this stream is locked to %d Hz (keep the rate consistent per session)"),
							Cmd.Samples.Num(), Cmd.SampleRate, EstablishedRate);
						break;
					}
					SherpaOnnxOnlineStreamAcceptWaveform(Stream, Cmd.SampleRate, Cmd.Samples.GetData(), Cmd.Samples.Num());
					PumpAndDispatch();
				}
				break;

			case FSttCommand::EType::Finish:
			{
				// Tail padding: without trailing right-context the last
				// word of the utterance gets truncated (upstream examples
				// pad the same way). MUST use the stream's established
				// rate -- a different rate here is a hard process exit
				// inside sherpa (features.cc resampler check).
				const int32 TailRate = EstablishedRate != 0 ? EstablishedRate : Recognizer->GetFeatSampleRate();
				TArray<float> TailSilence;
				TailSilence.AddZeroed(FMath::Max(1, (TailRate * 6) / 10)); // 0.6s
				SherpaOnnxOnlineStreamAcceptWaveform(Stream, TailRate, TailSilence.GetData(), TailSilence.Num());

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
	EstablishedRate = 0; // fresh stream = fresh feature extractor
	if (Stream == nullptr)
	{
		UE_LOG(LogInoSherpa, Error, TEXT("STT: stream recreation after Finish failed"));
		bStopRequested = true;
		DispatchSessionDead(); // owner drops the session instead of zombie-ing
	}
}

void FInoSttStreamWorker::DispatchPartial(const FString& Text) const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	const int32 Serial = SessionSerial;
	AsyncTask(ENamedThreads::GameThread, [Weak, Text, Serial]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyPartialFromWorker(Text, Serial);
		}
	});
}

void FInoSttStreamWorker::DispatchFinal(const FString& Text) const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	const int32 Serial = SessionSerial;
	AsyncTask(ENamedThreads::GameThread, [Weak, Text, Serial]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyFinalFromWorker(Text, Serial);
		}
	});
}

void FInoSttStreamWorker::DispatchEndpoint() const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	const int32 Serial = SessionSerial;
	AsyncTask(ENamedThreads::GameThread, [Weak, Serial]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyEndpointFromWorker(Serial);
		}
	});
}

void FInoSttStreamWorker::DispatchSessionDead() const
{
	TWeakObjectPtr<UInoSTT> Weak = WeakOwner;
	const int32 Serial = SessionSerial;
	AsyncTask(ENamedThreads::GameThread, [Weak, Serial]()
	{
		if (UInoSTT* Owner = Weak.Get())
		{
			Owner->NotifyWorkerDiedFromWorker(Serial);
		}
	});
}

#else // !WITH_INO_SHERPA -- stub platform, see InoSherpa.Build.cs

// Unreachable in practice: FInoSttRecognizer::Create always fails on stub
// platforms, so the subsystem never constructs a worker. Stream stays null
// -> IsHealthy() is false and the owner drops the worker immediately.
FInoSttStreamWorker::FInoSttStreamWorker(const TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe>& InRecognizer,
	const TWeakObjectPtr<UInoSTT>& InWeakOwner, int32 InSessionSerial)
	: Recognizer(InRecognizer)
	, WeakOwner(InWeakOwner)
	, SessionSerial(InSessionSerial)
{
	UE_LOG(LogInoSherpa, Error, TEXT("InoSherpa: sherpa-onnx is not built for this platform (Win64 only for now)"));
}

FInoSttStreamWorker::~FInoSttStreamWorker()
{
}

void FInoSttStreamWorker::EnqueuePush(TArray<float>&& Samples, int32 SampleRate)
{
}

void FInoSttStreamWorker::EnqueueFinish()
{
}

void FInoSttStreamWorker::EnqueueReset()
{
}

void FInoSttStreamWorker::Stop()
{
	bStopRequested = true;
}

uint32 FInoSttStreamWorker::Run()
{
	return 0;
}

#endif // WITH_INO_SHERPA
