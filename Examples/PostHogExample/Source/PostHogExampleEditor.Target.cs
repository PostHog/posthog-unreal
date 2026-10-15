using UnrealBuildTool;
public class PostHogExampleEditorTarget : TargetRules
{
    public PostHogExampleEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        ExtraModuleNames.Add("PostHogExample");
    }
}
