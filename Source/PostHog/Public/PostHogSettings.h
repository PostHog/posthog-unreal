#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PostHogClient.h"
#include "PostHogSettings.generated.h"

/** Complete runtime options: explicit setup does not inherit project credentials. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogOptions
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Connection")
    FString ProjectToken;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Connection")
    FString Host = TEXT("https://us.i.posthog.com");
    /** The subsystem adds a runtime/editor/PIE namespace to this name. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Connection")
    FString ClientName = TEXT("default");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
    bool bEnableInEditor = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
    bool bEnableOnDedicatedServer = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Privacy")
    EPostHogPersonProfiles PersonProfiles = EPostHogPersonProfiles::IdentifiedOnly;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Privacy")
    bool bInitiallyOptedOut = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lifecycle")
    EPostHogSessionMode SessionMode = EPostHogSessionMode::Automatic;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lifecycle")
    bool bCaptureApplicationLifecycleEvents = true;
    /** Empty uses Project Settings / Description / Project Version. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Application")
    FString AppVersion;
    /** Optional application build identifier supplied by the game. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Application")
    FString AppBuild;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flags")
    bool bPreloadFeatureFlags = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flags")
    bool bSendFeatureFlagEvents = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 FlushAt = 20;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 FlushIntervalMilliseconds = 30000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 MaxQueueSize = 1000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 MaxBatchSize = 50;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 RequestTimeoutSeconds = 10;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery", meta = (ClampMin = "1"))
    int32 ShutdownTimeoutMilliseconds = 3000;

    FPostHogConfig ToNative() const;
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "PostHog"))
class POSTHOG_API UPostHogSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPostHogSettings();
    /** Starts one client per game instance when Options has a project token. */
    UPROPERTY(Config, EditAnywhere, Category = "Setup")
    bool bAutoInitialize = true;
    UPROPERTY(Config, EditAnywhere, Category = "Setup")
    FPostHogOptions Options;
};
