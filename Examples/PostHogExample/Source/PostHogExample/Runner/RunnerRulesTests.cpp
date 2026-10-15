#include "RunnerRules.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunnerLaneTest, "PostHogExample.Runner.Lanes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRunnerLaneTest::RunTest(const FString&)
{
	TestEqual(TEXT("Left lane is bounded"), RunnerRules::ClampLane(-2), -1);
	TestEqual(TEXT("Right lane is bounded"), RunnerRules::ClampLane(2), 1);
	TestEqual(TEXT("Center lane"), RunnerRules::LaneY(0), 0.f);
	TestEqual(TEXT("Equal lane spacing"), RunnerRules::LaneY(1) - RunnerRules::LaneY(0),
		RunnerRules::LaneY(0) - RunnerRules::LaneY(-1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunnerSpeedTest, "PostHogExample.Runner.Speed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRunnerSpeedTest::RunTest(const FString&)
{
	TestEqual(TEXT("Initial speed"), RunnerRules::SpeedForDistance(0.f), 900.f);
	TestEqual(TEXT("No slowdown from negative distance"), RunnerRules::SpeedForDistance(-100.f), 900.f);
	TestTrue(TEXT("Speed increases with distance"), RunnerRules::SpeedForDistance(1000.f) > RunnerRules::SpeedForDistance(0.f));
	TestEqual(TEXT("Top speed is capped"), RunnerRules::SpeedForDistance(100000.f), 1800.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunnerTrackWindowTest, "PostHogExample.Runner.TrackWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRunnerTrackWindowTest::RunTest(const FString&)
{
	TestFalse(TEXT("Track beneath player stays alive"), RunnerRules::ShouldRetire(0.f, 1000.f));
	TestFalse(TEXT("Track boundary remains supported"), RunnerRules::ShouldRetire(0.f, 7200.f));
	TestTrue(TEXT("Track far behind player is retired"), RunnerRules::ShouldRetire(0.f, 7201.f));
	TestFalse(TEXT("Track ahead is never retired"), RunnerRules::ShouldRetire(20000.f, 1000.f));
	// Even a two-lane dodge at top speed fits between consecutive obstacle rows.
	TestTrue(TEXT("Rows allow time to cross both lanes"),
		RunnerRules::SegmentLength / RunnerRules::SpeedForDistance(100000.f) >
		2.f * RunnerRules::LaneWidth / 1500.f + 0.4f);
	return true;
}
#endif
