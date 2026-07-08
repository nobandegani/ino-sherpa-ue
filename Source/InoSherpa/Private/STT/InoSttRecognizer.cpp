// Copyright Inoland. All Rights Reserved.

#include "STT/InoSttRecognizer.h"

#include "InoSherpa.h"

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

TSharedPtr<FInoSttRecognizer, ESPMode::ThreadSafe> FInoSttRecognizer::Create(const FInoSTTModelConfig& Config, FString& OutError)
{
	if (Config.EncoderPath.IsEmpty() || Config.DecoderPath.IsEmpty() ||
		Config.JoinerPath.IsEmpty() || Config.TokensPath.IsEmpty())
	{
		OutError = TEXT("STT config requires EncoderPath, DecoderPath, JoinerPath and TokensPath");
		return nullptr;
	}

	FUtf8Args Utf8;
	SherpaOnnxOnlineRecognizerConfig SherpaConfig;
	FMemory::Memzero(SherpaConfig);

	SherpaConfig.feat_config.sample_rate = Config.SampleRate;
	SherpaConfig.feat_config.feature_dim = Config.FeatureDim;

	SherpaConfig.model_config.transducer.encoder = Utf8.Add(Config.EncoderPath);
	SherpaConfig.model_config.transducer.decoder = Utf8.Add(Config.DecoderPath);
	SherpaConfig.model_config.transducer.joiner  = Utf8.Add(Config.JoinerPath);
	SherpaConfig.model_config.tokens             = Utf8.Add(Config.TokensPath);
	SherpaConfig.model_config.num_threads        = FMath::Max(1, Config.NumThreads);
	SherpaConfig.model_config.provider           = InoSherpa::ProviderToString(Config.Provider);
	SherpaConfig.model_config.debug              = Config.bDebug ? 1 : 0;

	SherpaConfig.decoding_method  = Utf8.Add(Config.DecodingMethod);
	SherpaConfig.max_active_paths = FMath::Max(1, Config.MaxActivePaths);

	SherpaConfig.enable_endpoint             = Config.bEnableEndpoint ? 1 : 0;
	SherpaConfig.rule1_min_trailing_silence  = Config.Rule1MinTrailingSilence;
	SherpaConfig.rule2_min_trailing_silence  = Config.Rule2MinTrailingSilence;
	SherpaConfig.rule3_min_utterance_length  = Config.Rule3MinUtteranceLength;

	const double StartSeconds = FPlatformTime::Seconds();
	const SherpaOnnxOnlineRecognizer* Recognizer = SherpaOnnxCreateOnlineRecognizer(&SherpaConfig);
	if (Recognizer == nullptr)
	{
		OutError = TEXT("SherpaOnnxCreateOnlineRecognizer failed -- check encoder/decoder/joiner/tokens paths (enable bDebug for sherpa-side logs)");
		return nullptr;
	}

	UE_LOG(LogInoSherpa, Log, TEXT("STT: model loaded in %.2fs (feat rate=%d, dim=%d)"),
		FPlatformTime::Seconds() - StartSeconds, Config.SampleRate, Config.FeatureDim);

	return MakeShareable(new FInoSttRecognizer(Recognizer, Config.SampleRate));
}

FInoSttRecognizer::~FInoSttRecognizer()
{
	if (Recognizer != nullptr)
	{
		SherpaOnnxDestroyOnlineRecognizer(Recognizer);
		Recognizer = nullptr;
	}
}

FString FInoSttRecognizer::TranscribeOnce(TArrayView<const float> Samples, int32 SampleRate) const
{
	if (Samples.Num() == 0 || SampleRate <= 0)
	{
		return FString();
	}

	const SherpaOnnxOnlineStream* Stream = SherpaOnnxCreateOnlineStream(Recognizer);
	if (Stream == nullptr)
	{
		return FString();
	}

	SherpaOnnxOnlineStreamAcceptWaveform(Stream, SampleRate, Samples.GetData(), Samples.Num());

	// Tail padding: streaming models need trailing right-context or the
	// last word gets truncated (upstream examples do the same). MUST be at
	// the SAME rate as the audio above -- sherpa locks the stream's
	// resampler to the first rate it sees and process-EXITs on a mismatch
	// (features.cc).
	TArray<float> TailSilence;
	TailSilence.AddZeroed(FMath::Max(1, (SampleRate * 6) / 10)); // 0.6s
	SherpaOnnxOnlineStreamAcceptWaveform(Stream, SampleRate, TailSilence.GetData(), TailSilence.Num());

	SherpaOnnxOnlineStreamInputFinished(Stream);
	while (SherpaOnnxIsOnlineStreamReady(Recognizer, Stream))
	{
		SherpaOnnxDecodeOnlineStream(Recognizer, Stream);
	}

	FString Text;
	if (const SherpaOnnxOnlineRecognizerResult* Result = SherpaOnnxGetOnlineStreamResult(Recognizer, Stream))
	{
		Text = UTF8_TO_TCHAR(Result->text);
		SherpaOnnxDestroyOnlineRecognizerResult(Result);
	}

	SherpaOnnxDestroyOnlineStream(Stream);
	return Text;
}
