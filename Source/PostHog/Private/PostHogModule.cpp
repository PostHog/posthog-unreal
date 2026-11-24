#include "Modules/ModuleManager.h"
#include "PostHogClient.h"

class FPostHogModule final : public IModuleInterface
{
public:
    virtual bool SupportsDynamicReloading() override { return false; }
    virtual void ShutdownModule() override { FPostHogClient::ShutdownAll(); }
};

IMPLEMENT_MODULE(FPostHogModule, PostHog)
