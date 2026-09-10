// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"

INOSHERPA_API DECLARE_LOG_CATEGORY_EXTERN(LogInoSherpa, Log, All);

/**
 * InoSherpa module.
 *
 * sherpa-onnx (and its bundled CPU-only ONNX Runtime) is linked STATICALLY
 * into this module's binary, so unlike the DLL-staging sibling plugins
 * (InoOnnx / InoLlama / InoLiteRT) there is nothing to load here -- the
 * module exists to host the UInoTTS / UInoSTT subsystems and the shared
 * log category. LoadingPhase stays Default.
 */
class FInoSherpaModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
