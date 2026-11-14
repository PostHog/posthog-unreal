using UnrealBuildTool;
using System.IO;

public class PostHogRust : ModuleRules
{
    public PostHogRust(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));

        string Triple;
        string FileName = "libposthog_unreal_ffi.a";
        if (Target.Architectures.bIsMultiArch)
        {
            throw new BuildException("PostHog requires a single-architecture native artifact.");
        }
        if (Target.Platform == UnrealTargetPlatform.Mac && Target.Architecture == UnrealArch.Arm64)
        {
            Triple = "aarch64-apple-darwin";
            PublicFrameworks.AddRange(new[] { "Security", "SystemConfiguration", "CoreFoundation", "Foundation" });
            PublicSystemLibraries.AddRange(new[] { "resolv", "iconv" });
        }
        else if (Target.Platform == UnrealTargetPlatform.Win64 && Target.Architecture == UnrealArch.X64)
        {
            Triple = "x86_64-pc-windows-msvc";
            FileName = "posthog_unreal_ffi.lib";
            PublicSystemLibraries.AddRange(new[] {
                "advapi32.lib", "bcrypt.lib", "crypt32.lib", "ncrypt.lib",
                "ntdll.lib", "userenv.lib", "ws2_32.lib"
            });
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux && Target.Architecture == UnrealArch.X64)
        {
            Triple = "x86_64-unknown-linux-gnu";
            PublicSystemLibraries.AddRange(new[] { "gcc_s", "util", "rt", "pthread", "m", "dl" });
        }
        else
        {
            throw new BuildException("PostHog supports build targets Mac arm64, Win64 x64, and Linux x64.");
        }

        string Library = Path.Combine(ModuleDirectory, "lib", Triple, FileName);
        if (!File.Exists(Library))
        {
            throw new BuildException("PostHog Rust artifact is missing. Run python scripts/build-native.py " + Triple + " from the plugin source checkout.");
        }
        PublicAdditionalLibraries.Add(Library);

    }
}
