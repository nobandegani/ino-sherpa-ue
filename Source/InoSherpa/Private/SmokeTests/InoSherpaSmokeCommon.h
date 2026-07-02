// Copyright Inoland. All Rights Reserved.

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
