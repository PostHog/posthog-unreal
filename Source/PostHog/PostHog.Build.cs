using UnrealBuildTool;

public class PostHog : ModuleRules
{
    public PostHog(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Json", "Engine", "DeveloperSettings" });
        PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "RHI", "RenderCore", "ImageWrapper", "ImageCore", "PostHogRust" });
    }
}
