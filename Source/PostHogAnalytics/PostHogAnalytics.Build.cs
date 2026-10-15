using UnrealBuildTool;

public class PostHogAnalytics : ModuleRules
{
    public PostHogAnalytics(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "Analytics", "PostHog" });
    }
}
