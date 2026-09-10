// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "UObject/WeakObjectPtr.h"

class FInoSttRecognizer;
class UInoSTT;
typedef struct SherpaOnnxOnlineStream SherpaOnnxOnlineStream;

/**
 * Long-lived streaming STT worker (one per StartStream session).
 *
 * Exclusively owns one SherpaOnnxOnlineStream and is the ONLY thread that
 * touches it -- sherpa's online API has no internal locking, so
 * AcceptWaveform / Decode / GetResult / Reset on a stream must all be
 * serialized. The game thread produces commands into an SPSC queue; this
 * worker consumes them and dispatches partial / final / endpoint events
 * back via AsyncTask(GameThread) + a weak owner pointer.
 *
 * Modeled on InoAgents' FInoLiteRtLmConversationWorker.
 *
 * Lifecycle: constructed on the game thread (creates the stream + thread);
 * destructor joins the thread FIRST, then destroys the stream -- the
 * worker can never be mid-AcceptWaveform when the stream dies.
 */
class FInoSttStreamWorker : public FRunnable
{
public:
	FInoSttStreamWorker(const TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe>& InRecognizer,
		const TWeakObjectPtr<UInoSTT>& InWeakOwner, int32 InSessionSerial);
	virtual ~FInoSttStreamWorker() override;

	/** False when stream/thread creation failed; owner drops the worker. */
	bool IsHealthy() const { return Stream != nullptr && Thread.IsValid(); }

	// Game-thread producers.
	void EnqueuePush(TArray<float>&& Samples, int32 SampleRate);
	void EnqueueFinish();
	void EnqueueReset();

	//~ FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	struct FSttCommand
	{
		enum class EType : uint8 { PushAudio, Finish, Reset };
		EType Type = EType::PushAudio;
		TArray<float> Samples;
		int32 SampleRate = 0;
	};

	/** Drains the decoder, dispatches changed partials, handles endpoints. */
	void PumpAndDispatch();

	/** Current stream text (copied out; sherpa result freed immediately). */
	FString ReadResultText() const;

	/** Destroys + recreates the stream after InputFinished (a finished
	 *  stream accepts no more audio; recreation makes FinishStream ->
	 *  next utterance seamless). */
	void RecreateStream();

	void DispatchPartial(const FString& Text) const;
	void DispatchFinal(const FString& Text) const;
	void DispatchEndpoint() const;

	/** Worker hit an unrecoverable error; owner should drop the session. */
	void DispatchSessionDead() const;

	TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> Recognizer;
	TWeakObjectPtr<UInoSTT> WeakOwner;

	/** Stamped into every dispatch so the owner can drop events queued by
	 *  a previous session after a same-frame StopStream -> StartStream. */
	int32 SessionSerial = 0;

	/** Worker-thread-owned after construction. */
	const SherpaOnnxOnlineStream* Stream = nullptr;

	TQueue<FSttCommand, EQueueMode::Spsc> CommandQueue;
	FEvent* WakeEvent = nullptr;
	TUniquePtr<FRunnableThread> Thread;
	TAtomic<bool> bStopRequested{false};

	/** Worker-thread only: last partial text sent (anti-flood gate). */
	FString LastDispatchedText;

	/**
	 * Worker-thread only: the first sample rate this stream saw. sherpa's
	 * feature extractor keys its internal resampler to the FIRST rate and
	 * hard-EXITs the process on any later mismatch (features.cc), so the
	 * tail padding must use this rate and mismatched pushes are dropped.
	 * 0 until the first push; cleared on stream recreation.
	 */
	int32 EstablishedRate = 0;
};
