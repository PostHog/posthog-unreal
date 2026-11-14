using UnrealBuildTool;
public class PostHogTestHostEditorTarget : TargetRules
{
    public PostHogTestHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        ExtraModuleNames.Add("PostHogTestHost");
    }
}
