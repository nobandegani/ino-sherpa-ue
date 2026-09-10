// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "InoSherpaSettings.h"

#include "Misc/Paths.h"

UInoSherpaSettings::UInoSherpaSettings()
{
	// Defaults: public Hugging Face mirrors of the sherpa-onnx reference
	// models (same files the GitHub release archives contain). int8
	// variants where published -- best size/speed for on-device; the fp32
	// Zipformer decoder is tiny and upstream recipes keep it unquantized.

	StreamingSttModel.ModelDirName = TEXT("streaming-zipformer-en-2023-06-26");
	StreamingSttModel.Encoder.Url = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-2023-06-26/resolve/main/encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx");
	StreamingSttModel.Decoder.Url = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-2023-06-26/resolve/main/decoder-epoch-99-avg-1-chunk-16-left-128.onnx");
	StreamingSttModel.Joiner.Url  = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-2023-06-26/resolve/main/joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx");
	StreamingSttModel.Tokens.Url  = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-2023-06-26/resolve/main/tokens.txt");

	OfflineSttModel.ModelDirName = TEXT("nemo-parakeet-tdt-0.6b-v2-int8");
	OfflineSttModel.Encoder.Url = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8/resolve/main/encoder.int8.onnx");
	OfflineSttModel.Decoder.Url = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8/resolve/main/decoder.int8.onnx");
	OfflineSttModel.Joiner.Url  = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8/resolve/main/joiner.int8.onnx");
	OfflineSttModel.Tokens.Url  = TEXT("https://huggingface.co/csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8/resolve/main/tokens.txt");
}

const UInoSherpaSettings* UInoSherpaSettings::Get()
{
	return GetDefault<UInoSherpaSettings>();
}

FString UInoSherpaSettings::GetModelsRootDir()
{
	return FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("InoSherpa"));
}

FString UInoSherpaSettings::ResolveFileName(const FInoSherpaModelFileSource& File)
{
	if (!File.LocalFileName.IsEmpty())
	{
		return File.LocalFileName;
	}
	// Derive from the URL: strip query/fragment, take the last path segment.
	FString Name = File.Url;
	int32 CutIndex;
	if (Name.FindChar(TEXT('?'), CutIndex)) { Name.LeftInline(CutIndex); }
	if (Name.FindChar(TEXT('#'), CutIndex)) { Name.LeftInline(CutIndex); }
	int32 SlashIndex;
	if (Name.FindLastChar(TEXT('/'), SlashIndex)) { Name.RightChopInline(SlashIndex + 1); }
	return Name;
}

FString UInoSherpaSettings::ResolveModelDir(const FInoSherpaTransducerModelSource& Model)
{
	return FPaths::Combine(GetModelsRootDir(), Model.ModelDirName);
}

FString UInoSherpaSettings::ResolveFilePath(const FInoSherpaTransducerModelSource& Model, const FInoSherpaModelFileSource& File)
{
	return FPaths::Combine(ResolveModelDir(Model), ResolveFileName(File));
}

bool UInoSherpaSettings::IsModelDownloaded(const FInoSherpaTransducerModelSource& Model)
{
	for (const FInoSherpaModelFileSource* File : { &Model.Encoder, &Model.Decoder, &Model.Joiner, &Model.Tokens })
	{
		if (File->Url.IsEmpty() && File->LocalFileName.IsEmpty())
		{
			return false; // unconfigured slot
		}
		if (!FPaths::FileExists(ResolveFilePath(Model, *File)))
		{
			return false;
		}
	}
	return true;
}
