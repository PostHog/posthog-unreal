using UnrealBuildTool;
public class PostHogTestHost : ModuleRules
{
    public PostHogTestHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "PostHog" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "RHI", "Json", "Analytics" });
    }
}
