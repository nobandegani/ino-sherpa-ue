// Copyright Inoland. All Rights Reserved.

#include "InoSherpa.h"

#include "sherpa-onnx/c-api/c-api.h"

DEFINE_LOG_CATEGORY(LogInoSherpa);

void FInoSherpaModule::StartupModule()
{
	// Statically linked -- this call doubles as the link-sanity check: if
	// the 14 staged .lib files didn't resolve, we'd never get here.
	UE_LOG(LogInoSherpa, Log, TEXT("Sherpa: Module up. sherpa-onnx %hs (git %hs)"),
		SherpaOnnxGetVersionStr(), SherpaOnnxGetGitSha1());
}

void FInoSherpaModule::ShutdownModule()
{
	UE_LOG(LogInoSherpa, Log, TEXT("Sherpa: Module down."));
}

IMPLEMENT_MODULE(FInoSherpaModule, InoSherpa)
