using UnrealBuildTool;
public class PostHogExampleTarget : TargetRules
{
    public PostHogExampleTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        ExtraModuleNames.Add("PostHogExample");
    }
}
