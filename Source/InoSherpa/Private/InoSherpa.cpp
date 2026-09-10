// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "InoSherpa.h"

#if WITH_INO_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

DEFINE_LOG_CATEGORY(LogInoSherpa);

void FInoSherpaModule::StartupModule()
{
#if WITH_INO_SHERPA
	// Statically linked -- this call doubles as the link-sanity check: if
	// the 14 staged .lib files didn't resolve, we'd never get here.
	UE_LOG(LogInoSherpa, Log, TEXT("Sherpa: Module up. sherpa-onnx %hs (git %hs)"),
		SherpaOnnxGetVersionStr(), SherpaOnnxGetGitSha1());
#else
	// Stub platform (WITH_INO_SHERPA=0): subsystems exist but every model
	// load fails gracefully. See InoSherpa.Build.cs.
	UE_LOG(LogInoSherpa, Log, TEXT("Sherpa: Module up (stub -- sherpa-onnx not built for this platform)."));
#endif
}

void FInoSherpaModule::ShutdownModule()
{
	UE_LOG(LogInoSherpa, Log, TEXT("Sherpa: Module down."));
}

IMPLEMENT_MODULE(FInoSherpaModule, InoSherpa)
