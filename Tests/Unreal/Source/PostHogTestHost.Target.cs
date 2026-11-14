using UnrealBuildTool;
public class PostHogTestHostTarget : TargetRules
{
    public PostHogTestHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        ExtraModuleNames.Add("PostHogTestHost");
    }
}
