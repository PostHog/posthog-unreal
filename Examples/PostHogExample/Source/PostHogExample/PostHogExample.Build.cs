using UnrealBuildTool;
public class PostHogExample : ModuleRules
{
    public PostHogExample(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "PostHog" });
    }
}
