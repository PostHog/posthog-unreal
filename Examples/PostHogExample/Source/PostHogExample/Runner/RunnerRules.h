#pragma once

#include "CoreMinimal.h"

/** Shared dimensions and deterministic rules for the runner. Distances are in centimeters. */
namespace RunnerRules
{
	constexpr float LaneWidth = 300.f;
	constexpr float SegmentLength = 2400.f;
	constexpr float LookAhead = 18000.f;
	constexpr float KeepBehind = 4800.f;
	/** Corner tiles are squares; the centre line is a quarter arc of half the tile's side. */
	constexpr float CornerSize = 1200.f;
	constexpr float CornerRadius = CornerSize / 2.f;
	/** How far a straight's side scenery reaches from its centre line; turns can bring other pieces within it. */
	constexpr float SceneryReach = 5000.f;
	constexpr int32 CourseSeed = 90731;
	constexpr int32 OpeningStraights = 4;
	constexpr int32 MinStraights = 4;
	constexpr int32 MaxStraights = 7;
	static_assert(MinStraights >= OpeningStraights, "The opening straights must precede the first corner");
	static_assert(CornerRadius - LaneWidth >= 250.f, "The inner lane needs a usable turning radius");

	inline int32 ClampLane(int32 Lane) { return FMath::Clamp(Lane, -1, 1); }
	inline float LaneY(int32 Lane) { return ClampLane(Lane) * LaneWidth; }
	inline float SpeedForDistance(float Meters) { return FMath::Clamp(900.f + FMath::Max(0.f, Meters) * 0.3f, 900.f, 1800.f); }
	inline bool ShouldRetire(float SegmentStart, float PlayerX)
	{
		return SegmentStart + SegmentLength < PlayerX - KeepBehind;
	}

	/**
	 * Deterministic course layout. Next() returns 0 for a straight segment, or +1 / -1 for a right / left corner.
	 * Corners follow every MinStraights..MaxStraights straights and never turn the same way three times in a row,
	 * so the live track window cannot cross itself.
	 */
	class FCourse
	{
	public:
		explicit FCourse(int32 Seed = CourseSeed) : Random(Seed), StraightsLeft(Random.RandRange(MinStraights, MaxStraights)) {}

		int32 Next()
		{
			if (StraightsLeft > 0)
			{
				--StraightsLeft;
				return 0;
			}
			int32 Turn = Random.RandRange(0, 1) ? 1 : -1;
			if (Turn == LastTurn && SameTurns >= 2) Turn = -Turn;
			SameTurns = Turn == LastTurn ? SameTurns + 1 : 1;
			LastTurn = Turn;
			StraightsLeft = Random.RandRange(MinStraights, MaxStraights);
			return Turn;
		}

	private:
		FRandomStream Random;
		int32 StraightsLeft;
		int32 LastTurn = 0;
		int32 SameTurns = 0;
	};
}
