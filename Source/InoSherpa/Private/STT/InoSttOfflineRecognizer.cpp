// Copyright Inoland. All Rights Reserved.

#include "STT/InoSttOfflineRecognizer.h"

#include "InoSherpa.h"

#if WITH_INO_SHERPA

#include "sherpa-onnx/c-api/c-api.h"

namespace
{
/** FString -> UTF-8 arguments kept alive through a sherpa Create call. */
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
} // namespace

TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> FInoSttOfflineRecognizer::Create(const FInoSTTOfflineModelConfig& Config, FString& OutError)
{
	// Validate the selected family up front -- sherpa's own failure mode is
	// a NULL handle plus stderr output UE never shows.
	switch (Config.ModelType)
	{
	case EInoSTTOfflineModelType::NemoTransducer:
		if (Config.Transducer.EncoderPath.IsEmpty() || Config.Transducer.DecoderPath.IsEmpty() ||
			Config.Transducer.JoinerPath.IsEmpty() || Config.TokensPath.IsEmpty())
		{
			OutError = TEXT("NemoTransducer config requires Transducer.{Encoder,Decoder,Joiner}Path and TokensPath");
			return nullptr;
		}
		break;
	case EInoSTTOfflineModelType::Whisper:
		if (Config.Whisper.EncoderPath.IsEmpty() || Config.Whisper.DecoderPath.IsEmpty() || Config.TokensPath.IsEmpty())
		{
			OutError = TEXT("Whisper config requires Whisper.{Encoder,Decoder}Path and TokensPath");
			return nullptr;
		}
		break;
	case EInoSTTOfflineModelType::SenseVoice:
		if (Config.SenseVoice.ModelPath.IsEmpty() || Config.TokensPath.IsEmpty())
		{
			OutError = TEXT("SenseVoice config requires SenseVoice.ModelPath and TokensPath");
			return nullptr;
		}
		break;
	case EInoSTTOfflineModelType::Moonshine:
		if (Config.Moonshine.PreprocessorPath.IsEmpty() || Config.Moonshine.EncoderPath.IsEmpty() ||
			Config.Moonshine.UncachedDecoderPath.IsEmpty() || Config.Moonshine.CachedDecoderPath.IsEmpty() ||
			Config.TokensPath.IsEmpty())
		{
			OutError = TEXT("Moonshine config requires all four Moonshine.*Path fields and TokensPath");
			return nullptr;
		}
		break;
	default:
		OutError = TEXT("Unknown offline STT model type");
		return nullptr;
	}

	FUtf8Args Utf8;
	SherpaOnnxOfflineRecognizerConfig SherpaConfig;
	FMemory::Memzero(SherpaConfig);

	SherpaConfig.feat_config.sample_rate = Config.SampleRate;
	SherpaConfig.feat_config.feature_dim = Config.FeatureDim;

	switch (Config.ModelType)
	{
	case EInoSTTOfflineModelType::NemoTransducer:
		SherpaConfig.model_config.transducer.encoder = Utf8.Add(Config.Transducer.EncoderPath);
		SherpaConfig.model_config.transducer.decoder = Utf8.Add(Config.Transducer.DecoderPath);
		SherpaConfig.model_config.transducer.joiner  = Utf8.Add(Config.Transducer.JoinerPath);
		// Distinguishes NeMo/Parakeet transducers from icefall ones (see
		// the Parakeet example in c-api.h).
		SherpaConfig.model_config.model_type = "nemo_transducer";
		break;
	case EInoSTTOfflineModelType::Whisper:
		SherpaConfig.model_config.whisper.encoder  = Utf8.Add(Config.Whisper.EncoderPath);
		SherpaConfig.model_config.whisper.decoder  = Utf8.Add(Config.Whisper.DecoderPath);
		SherpaConfig.model_config.whisper.language = Utf8.Add(Config.Whisper.Language);
		SherpaConfig.model_config.whisper.task     = Utf8.Add(Config.Whisper.Task);
		break;
	case EInoSTTOfflineModelType::SenseVoice:
		SherpaConfig.model_config.sense_voice.model    = Utf8.Add(Config.SenseVoice.ModelPath);
		SherpaConfig.model_config.sense_voice.language = Utf8.Add(Config.SenseVoice.Language);
		SherpaConfig.model_config.sense_voice.use_itn  = Config.SenseVoice.bUseItn ? 1 : 0;
		break;
	case EInoSTTOfflineModelType::Moonshine:
	default:
		SherpaConfig.model_config.moonshine.preprocessor     = Utf8.Add(Config.Moonshine.PreprocessorPath);
		SherpaConfig.model_config.moonshine.encoder          = Utf8.Add(Config.Moonshine.EncoderPath);
		SherpaConfig.model_config.moonshine.uncached_decoder = Utf8.Add(Config.Moonshine.UncachedDecoderPath);
		SherpaConfig.model_config.moonshine.cached_decoder   = Utf8.Add(Config.Moonshine.CachedDecoderPath);
		break;
	}

	SherpaConfig.model_config.tokens      = Utf8.Add(Config.TokensPath);
	SherpaConfig.model_config.num_threads = FMath::Max(1, Config.NumThreads);
	SherpaConfig.model_config.provider    = InoSherpa::ProviderToString(Config.Provider);
	SherpaConfig.model_config.debug       = Config.bDebug ? 1 : 0;

	SherpaConfig.decoding_method  = Utf8.Add(Config.DecodingMethod);
	SherpaConfig.max_active_paths = FMath::Max(1, Config.MaxActivePaths);

	const double StartSeconds = FPlatformTime::Seconds();
	const SherpaOnnxOfflineRecognizer* Recognizer = SherpaOnnxCreateOfflineRecognizer(&SherpaConfig);
	if (Recognizer == nullptr)
	{
		OutError = TEXT("SherpaOnnxCreateOfflineRecognizer failed -- check model/tokens paths (enable bDebug for sherpa-side logs)");
		return nullptr;
	}

	UE_LOG(LogInoSherpa, Log, TEXT("STT: offline model loaded in %.2fs (threads=%d)"),
		FPlatformTime::Seconds() - StartSeconds, FMath::Max(1, Config.NumThreads));

	return MakeShareable(new FInoSttOfflineRecognizer(Recognizer));
}

FInoSttOfflineRecognizer::~FInoSttOfflineRecognizer()
{
	if (Recognizer != nullptr)
	{
		SherpaOnnxDestroyOfflineRecognizer(Recognizer);
		Recognizer = nullptr;
	}
}

FString FInoSttOfflineRecognizer::Transcribe(TArrayView<const float> Samples, int32 SampleRate) const
{
	if (Samples.Num() == 0 || SampleRate <= 0)
	{
		return FString();
	}

	const SherpaOnnxOfflineStream* Stream = SherpaOnnxCreateOfflineStream(Recognizer);
	if (Stream == nullptr)
	{
		return FString();
	}

	// Whole utterance in ONE call (the offline API's contract), then decode.
	SherpaOnnxAcceptWaveformOffline(Stream, SampleRate, Samples.GetData(), Samples.Num());
	SherpaOnnxDecodeOfflineStream(Recognizer, Stream);

	FString Text;
	if (const SherpaOnnxOfflineRecognizerResult* Result = SherpaOnnxGetOfflineStreamResult(Stream))
	{
		Text = UTF8_TO_TCHAR(Result->text);
		SherpaOnnxDestroyOfflineRecognizerResult(Result);
	}

	SherpaOnnxDestroyOfflineStream(Stream);
	return Text;
}

#else // !WITH_INO_SHERPA -- stub platform, see InoSherpa.Build.cs

TSharedPtr<FInoSttOfflineRecognizer, ESPMode::ThreadSafe> FInoSttOfflineRecognizer::Create(const FInoSTTOfflineModelConfig& Config, FString& OutError)
{
	OutError = TEXT("InoSherpa: sherpa-onnx is not built for this platform (Win64 only for now)");
	return nullptr;
}

FInoSttOfflineRecognizer::~FInoSttOfflineRecognizer()
{
}

FString FInoSttOfflineRecognizer::Transcribe(TArrayView<const float> Samples, int32 SampleRate) const
{
	return FString();
}

#endif // WITH_INO_SHERPA
