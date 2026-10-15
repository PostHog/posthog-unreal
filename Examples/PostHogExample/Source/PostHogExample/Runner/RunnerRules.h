#pragma once

#include "CoreMinimal.h"

/** Shared dimensions and deterministic rules for the runner. Distances are in centimeters. */
namespace RunnerRules
{
	constexpr float LaneWidth = 300.f;
	constexpr float SegmentLength = 2400.f;
	constexpr float LookAhead = 18000.f;
	constexpr float KeepBehind = 4800.f;

	inline int32 ClampLane(int32 Lane) { return FMath::Clamp(Lane, -1, 1); }
	inline float LaneY(int32 Lane) { return ClampLane(Lane) * LaneWidth; }
	inline float SpeedForDistance(float Meters) { return FMath::Clamp(900.f + FMath::Max(0.f, Meters) * 0.3f, 900.f, 1800.f); }
	inline bool ShouldRetire(float SegmentStart, float PlayerX)
	{
		return SegmentStart + SegmentLength < PlayerX - KeepBehind;
	}
}
