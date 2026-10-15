#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IAnalyticsProvider.h"
#include "PostHogClient.h"

/** Unreal Analytics adapter over a game-instance-owned client.
 * Releasing an adapter does not shut down the client. Retained adapters close with
 * their owner and never retarget a replacement client. Event methods are thread-safe;
 * world lookup and default-provider binding are game-thread-only.
 */
class POSTHOG_API FPostHogAnalyticsProvider final : public IAnalyticsProvider
{
public:
    /** Resolves World -> GameInstance -> PostHogSubsystem. Null until setup succeeds. */
    static TSharedPtr<IAnalyticsProvider> GetForWorld(const UObject* WorldContextObject);
    /** Used by the PostHogAnalytics module; never selects a world implicitly. */
    static TSharedPtr<IAnalyticsProvider> GetBoundProvider();

    using IAnalyticsProvider::RecordEvent;
    using IAnalyticsProvider::StartSession;

    virtual bool StartSession(const TArray<FAnalyticsEventAttribute>& Attributes) override;
    virtual void EndSession() override;
    virtual FString GetSessionID() const override;
    virtual bool SetSessionID(const FString& InSessionID) override;
    virtual void FlushEvents() override;
    virtual void SetUserID(const FString& InUserID) override;
    virtual FString GetUserID() const override;
    virtual void RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes) override;
    virtual void RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes, EAnalyticsRecordEventMode Mode) override;
    virtual void SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes) override;
    virtual TArray<FAnalyticsEventAttribute> GetDefaultEventAttributesSafe() const override;
    virtual int32 GetDefaultEventAttributeCount() const override;
    virtual FAnalyticsEventAttribute GetDefaultEventAttribute(int AttributeIndex) const override;

private:
    friend class UPostHogSubsystem;
    explicit FPostHogAnalyticsProvider(FPostHogClient::FPtr InClient) : Client(MoveTemp(InClient)) {}
    static bool Bind(const TSharedPtr<FPostHogAnalyticsProvider>& Provider);
    void Unbind();
    FPostHogProperties GetDefaults() const;
    static FPostHogProperties ConvertAttributes(const TArray<FAnalyticsEventAttribute>& Attributes);

    FPostHogClient::FPtr Client;
    mutable FCriticalSection DefaultsMutex;
    TArray<FAnalyticsEventAttribute> DefaultAttributes;
    FPostHogProperties DefaultProperties;
};
