// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "InoSherpaSettings.generated.h"

/**
 * One downloadable model file (Project Settings -> Plugins -> InoSherpa).
 * sherpa models are multi-file, so a model source is a set of these.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoSherpaModelFileSource
{
	GENERATED_BODY()

	/**
	 * Direct download URL. For Hugging Face:
	 *   https://huggingface.co/<org>/<repo>/resolve/main/<file>
	 * Anything FHttpModule can GET works (Hugging Face, S3, your CDN).
	 * Public URLs only -- no auth handling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FString Url;

	/** Filename to save as. Empty = derive from the URL's last path segment. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FString LocalFileName;

	/**
	 * Hex-encoded SHA-256 for integrity verification. Optional but
	 * recommended for shipped configs. Empty = skip verification.
	 * Lowercase, 64 chars, no separators (same format as `sha256sum`).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model", meta = (DisplayName = "Expected SHA-256"))
	FString ExpectedSha256;

	/**
	 * Total bytes -- progress fallback when the server omits
	 * Content-Length. 0 = the downloader HEAD-probes before the GET.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	int64 FileSizeBytes = 0;
};

/**
 * A transducer STT model source: the four files every sherpa transducer
 * needs (streaming Zipformer and offline NeMo/Parakeet share this shape).
 * Files land at:
 *   <FPaths::ProjectPersistentDownloadDir()>/InoSherpa/<ModelDirName>/<file>
 * Already-present files are skipped (the downloader's cached check), so
 * the download step is a cheap no-op after first run.
 */
USTRUCT(BlueprintType)
struct INOSHERPA_API FInoSherpaTransducerModelSource
{
	GENERATED_BODY()

	/** Subdirectory under <persistent-downloads>/InoSherpa/ for this model. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FString ModelDirName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FInoSherpaModelFileSource Encoder;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FInoSherpaModelFileSource Decoder;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FInoSherpaModelFileSource Joiner;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Model")
	FInoSherpaModelFileSource Tokens;
};

/**
 * Project Settings -> Plugins -> InoSherpa.
 *
 * Model download sources for the two STT models. UInoSTT's
 * LoadStreamingModelFromSettingsAsync / LoadOfflineModelFromSettingsAsync
 * download whatever is missing (via the InoNodes downloader: resume,
 * retries, SHA-256, cancel) and then load the model. Defaults point at the
 * public Hugging Face mirrors of the sherpa-onnx reference models --
 * override with your own CDN for shipping.
 *
 * TTS (Piper) is not settings-driven yet: Piper bundles include the
 * espeak-ng-data DIRECTORY (hundreds of small files), which doesn't fit
 * the per-file download model -- needs an archive step first.
 */
UCLASS(Config = Game, defaultconfig, meta = (DisplayName = "InoSherpa"))
class INOSHERPA_API UInoSherpaSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UInoSherpaSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Streaming STT (live partials while talking) -- Zipformer transducer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "STT Streaming Model (Zipformer)")
	FInoSherpaTransducerModelSource StreamingSttModel;

	/** Offline STT (whole-utterance, higher accuracy) -- Parakeet-TDT. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "STT Offline Model (Parakeet)")
	FInoSherpaTransducerModelSource OfflineSttModel;

	// ---- C++ helpers -------------------------------------------------------

	static const UInoSherpaSettings* Get();

	/** <ProjectPersistentDownloadDir>/InoSherpa */
	static FString GetModelsRootDir();

	/** Local filename for a file source (explicit name or derived from URL). */
	static FString ResolveFileName(const FInoSherpaModelFileSource& File);

	/** Absolute directory a model source downloads into. */
	static FString ResolveModelDir(const FInoSherpaTransducerModelSource& Model);

	/** Absolute path one file of a model source lands at. */
	static FString ResolveFilePath(const FInoSherpaTransducerModelSource& Model, const FInoSherpaModelFileSource& File);

	/** True when all four files of the model exist on disk (cheap stat probe -- UMG-safe). */
	static bool IsModelDownloaded(const FInoSherpaTransducerModelSource& Model);
};
