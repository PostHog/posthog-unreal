#pragma once

#include "CoreMinimal.h"

class UWorld;

/** Spawns the lighting, sky, fog, and static scenery that surround the track. */
namespace RunnerEnvironment
{
	void Spawn(UWorld* World);
}
