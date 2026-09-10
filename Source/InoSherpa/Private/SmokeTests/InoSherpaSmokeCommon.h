// Copyright 2026 Inoland.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

/**
 * Shared bits for the Ino.Sherpa.* console smoke tests. The subsystems live
 * on the game instance, so every command needs a running PIE / game world.
 */
namespace InoSherpaSmoke
{
inline UGameInstance* FindGameInstance()
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if ((Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game) &&
			Context.OwningGameInstance != nullptr)
		{
			return Context.OwningGameInstance;
		}
	}
	return nullptr;
}
} // namespace InoSherpaSmoke
