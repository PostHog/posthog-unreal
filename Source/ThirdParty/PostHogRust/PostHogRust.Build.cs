using UnrealBuildTool;
using System.IO;

public class PostHogRust : ModuleRules
{
    public PostHogRust(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));

        if (Target.Platform != UnrealTargetPlatform.Mac || Target.Architectures.bIsMultiArch || Target.Architecture != UnrealArch.Arm64)
        {
            throw new BuildException("PostHog native integration currently requires a Mac arm64 target. Other desktop targets require native linking and lifecycle validation first.");
        }

        string Library = Path.Combine(ModuleDirectory, "lib", "aarch64-apple-darwin", "libposthog_unreal_ffi.a");
        if (!File.Exists(Library))
        {
            throw new BuildException("PostHog Rust artifact is missing. Run scripts/build-native.sh aarch64-apple-darwin from the plugin source checkout.");
        }
        PublicAdditionalLibraries.Add(Library);
        PublicFrameworks.AddRange(new string[] { "Security", "SystemConfiguration", "CoreFoundation", "Foundation" });
        PublicSystemLibraries.AddRange(new string[] { "resolv", "iconv" });
    }
}
