// Copyright Inoland. All Rights Reserved.

#include "TTS/InoTtsEngine.h"

#include "InoSherpa.h"
#include "InoSherpaPcm.h"

#if WITH_INO_SHERPA

#include "sherpa-onnx/c-api/c-api.h"

namespace
{
/**
 * Keeps FString -> UTF-8 conversions alive for the duration of a sherpa
 * Create call (the config structs hold raw const char* into these buffers).
 * Empty FStrings map to nullptr, matching sherpa's memset-to-zero
 * convention for unset fields.
 */
class FUtf8Args
{
public:
	const char* Add(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return nullptr;
		}
		Held.Add(MakeUnique<FTCHARToUTF8>(*Value));
		return reinterpret_cast<const char*>(Held.Last()->Get());
	}

private:
	TArray<TUniquePtr<FTCHARToUTF8>> Held;
};

struct FGenerateContext
{
	std::atomic<bool>* CancelFlag = nullptr;
	const FInoTtsEngine::FChunkFn* ChunkFn = nullptr;
	bool bCancelled = false;
};

int32_t GenerateTrampoline(const float* Samples, int32_t NumSamples, float Progress, void* Arg)
{
	FGenerateContext* Ctx = static_cast<FGenerateContext*>(Arg);
	if (Ctx->CancelFlag && Ctx->CancelFlag->load(std::memory_order_acquire))
	{
		Ctx->bCancelled = true;
		return 0; // abort generation; GenerateWithConfig returns the partial audio
	}
	if (Ctx->ChunkFn && (*Ctx->ChunkFn) && NumSamples > 0)
	{
		// Samples are only valid during this callback -- convert (copies) now.
		TArray<uint8> Bytes;
		InoSherpaPcm::Float32ToInt16PcmBytesMono(MakeArrayView(Samples, NumSamples), Bytes);
		(*Ctx->ChunkFn)(MoveTemp(Bytes), Progress);
	}
	return 1;
}
} // namespace

TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> FInoTtsEngine::Create(const FInoTTSModelConfig& Config, FString& OutError)
{
	// Validate before handing to sherpa -- its own failure mode is a NULL
	// handle plus stderr output UE never shows.
	switch (Config.ModelType)
	{
	case EInoTTSModelType::PiperVits:
		if (Config.Piper.ModelPath.IsEmpty() || Config.Piper.TokensPath.IsEmpty())
		{
			OutError = TEXT("Piper config requires ModelPath and TokensPath");
			return nullptr;
		}
		if (Config.Piper.DataDir.IsEmpty() && Config.Piper.LexiconPath.IsEmpty())
		{
			OutError = TEXT("Piper config requires DataDir (espeak-ng-data) or LexiconPath");
			return nullptr;
		}
		break;
	case EInoTTSModelType::Kokoro:
		if (Config.Kokoro.ModelPath.IsEmpty() || Config.Kokoro.VoicesPath.IsEmpty() || Config.Kokoro.TokensPath.IsEmpty())
		{
			OutError = TEXT("Kokoro config requires ModelPath, VoicesPath and TokensPath");
			return nullptr;
		}
		break;
	default:
		OutError = TEXT("Unknown TTS model type");
		return nullptr;
	}

	FUtf8Args Utf8;
	SherpaOnnxOfflineTtsConfig SherpaConfig;
	FMemory::Memzero(SherpaConfig);

	switch (Config.ModelType)
	{
	case EInoTTSModelType::PiperVits:
		SherpaConfig.model.vits.model         = Utf8.Add(Config.Piper.ModelPath);
		SherpaConfig.model.vits.tokens        = Utf8.Add(Config.Piper.TokensPath);
		SherpaConfig.model.vits.data_dir      = Utf8.Add(Config.Piper.DataDir);
		SherpaConfig.model.vits.lexicon       = Utf8.Add(Config.Piper.LexiconPath);
		SherpaConfig.model.vits.noise_scale   = Config.Piper.NoiseScale;
		SherpaConfig.model.vits.noise_scale_w = Config.Piper.NoiseScaleW;
		SherpaConfig.model.vits.length_scale  = Config.Piper.LengthScale;
		break;
	case EInoTTSModelType::Kokoro:
	default:
		SherpaConfig.model.kokoro.model        = Utf8.Add(Config.Kokoro.ModelPath);
		SherpaConfig.model.kokoro.voices       = Utf8.Add(Config.Kokoro.VoicesPath);
		SherpaConfig.model.kokoro.tokens       = Utf8.Add(Config.Kokoro.TokensPath);
		SherpaConfig.model.kokoro.data_dir     = Utf8.Add(Config.Kokoro.DataDir);
		SherpaConfig.model.kokoro.lexicon      = Utf8.Add(Config.Kokoro.LexiconPath);
		SherpaConfig.model.kokoro.lang         = Utf8.Add(Config.Kokoro.Lang);
		SherpaConfig.model.kokoro.length_scale = Config.Kokoro.LengthScale;
		break;
	}

	SherpaConfig.model.num_threads = FMath::Max(1, Config.NumThreads);
	SherpaConfig.model.debug       = Config.bDebug ? 1 : 0;
	SherpaConfig.model.provider    = InoSherpa::ProviderToString(Config.Provider);
	SherpaConfig.max_num_sentences = FMath::Max(1, Config.MaxNumSentences);
	SherpaConfig.silence_scale     = Config.SilenceScale;
	SherpaConfig.rule_fsts         = Utf8.Add(Config.RuleFsts);

	const double StartSeconds = FPlatformTime::Seconds();
	const SherpaOnnxOfflineTts* Tts = SherpaOnnxCreateOfflineTts(&SherpaConfig);
	if (Tts == nullptr)
	{
		OutError = TEXT("SherpaOnnxCreateOfflineTts failed -- check model/tokens/data-dir paths (enable bDebug for sherpa-side logs)");
		return nullptr;
	}

	UE_LOG(LogInoSherpa, Log, TEXT("TTS: model loaded in %.2fs (rate=%d, speakers=%d)"),
		FPlatformTime::Seconds() - StartSeconds,
		SherpaOnnxOfflineTtsSampleRate(Tts), SherpaOnnxOfflineTtsNumSpeakers(Tts));

	return MakeShareable(new FInoTtsEngine(Tts));
}

FInoTtsEngine::~FInoTtsEngine()
{
	if (Tts != nullptr)
	{
		SherpaOnnxDestroyOfflineTts(Tts);
		Tts = nullptr;
	}
}

int32 FInoTtsEngine::GetSampleRate() const
{
	return Tts ? SherpaOnnxOfflineTtsSampleRate(Tts) : 0;
}

int32 FInoTtsEngine::GetNumSpeakers() const
{
	return Tts ? SherpaOnnxOfflineTtsNumSpeakers(Tts) : 0;
}

FInoTTSResult FInoTtsEngine::Generate(const FString& Text, const FInoTTSOptions& Options,
	const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancelFlag,
	const FChunkFn& ChunkFn) const
{
	FInoTTSResult Result;

	if (Text.TrimStartAndEnd().IsEmpty())
	{
		Result.ErrorMessage = TEXT("empty text");
		return Result;
	}

	SherpaOnnxGenerationConfig Gen;
	FMemory::Memzero(Gen);
	Gen.speed         = Options.Speed;
	Gen.sid           = Options.SpeakerId;
	Gen.silence_scale = Options.SilenceScale;

	FGenerateContext Ctx;
	Ctx.CancelFlag = CancelFlag.IsValid() ? CancelFlag.Get() : nullptr;
	Ctx.ChunkFn    = &ChunkFn;

	// Only install the callback when it has a job -- with neither cancel
	// nor streaming, a NULL callback skips per-chunk overhead entirely.
	const bool bNeedCallback = (Ctx.CancelFlag != nullptr) || static_cast<bool>(ChunkFn);

	const FTCHARToUTF8 TextUtf8(*Text);
	const double StartSeconds = FPlatformTime::Seconds();

	const SherpaOnnxGeneratedAudio* Audio = SherpaOnnxOfflineTtsGenerateWithConfig(
		Tts, reinterpret_cast<const char*>(TextUtf8.Get()), &Gen,
		bNeedCallback ? &GenerateTrampoline : nullptr,
		bNeedCallback ? &Ctx : nullptr);

	Result.GenerationTimeSeconds = static_cast<float>(FPlatformTime::Seconds() - StartSeconds);

	if (Audio == nullptr)
	{
		Result.ErrorMessage = TEXT("SherpaOnnxOfflineTtsGenerateWithConfig returned null");
		return Result;
	}

	Result.bSuccess     = true;
	Result.bWasCancelled = Ctx.bCancelled;
	Result.SampleRate   = Audio->sample_rate;
	if (Audio->n > 0)
	{
		InoSherpaPcm::Float32ToInt16PcmBytesMono(MakeArrayView(Audio->samples, Audio->n), Result.AudioSamples);
		Result.DurationSeconds = static_cast<float>(Audio->n) / static_cast<float>(FMath::Max(1, Audio->sample_rate));
	}
	Result.RealTimeFactor = Result.DurationSeconds > 0.0f
		? Result.GenerationTimeSeconds / Result.DurationSeconds
		: 0.0f;

	SherpaOnnxDestroyOfflineTtsGeneratedAudio(Audio);
	return Result;
}

#else // !WITH_INO_SHERPA -- stub platform, see InoSherpa.Build.cs

TSharedPtr<FInoTtsEngine, ESPMode::ThreadSafe> FInoTtsEngine::Create(const FInoTTSModelConfig& Config, FString& OutError)
{
	OutError = TEXT("InoSherpa: sherpa-onnx is not built for this platform (Win64 only for now)");
	return nullptr;
}

FInoTtsEngine::~FInoTtsEngine()
{
}

int32 FInoTtsEngine::GetSampleRate() const
{
	return 0;
}

int32 FInoTtsEngine::GetNumSpeakers() const
{
	return 0;
}

FInoTTSResult FInoTtsEngine::Generate(const FString& Text, const FInoTTSOptions& Options,
	const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancelFlag,
	const FChunkFn& ChunkFn) const
{
	FInoTTSResult Result;
	Result.ErrorMessage = TEXT("InoSherpa: sherpa-onnx is not built for this platform (Win64 only for now)");
	return Result;
}

#endif // WITH_INO_SHERPA
