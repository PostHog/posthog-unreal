#include "PostHogTasks.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include <atomic>

namespace
{
class FPostHogTask final : public FRunnable
{
public:
    TFunction<void()> Work;
    FRunnableThread* Thread = nullptr;
    std::atomic<bool> Complete{false};
    virtual uint32 Run() override
    {
        Work();
        Work = nullptr;
        Complete.store(true, std::memory_order_release);
        return 0;
    }
    ~FPostHogTask()
    {
        if (Thread) { Thread->WaitForCompletion(); delete Thread; }
    }
};
FCriticalSection TasksMutex;
TArray<TUniquePtr<FPostHogTask>> Tasks;
}

bool PostHogTasks::Launch(TFunction<void()>&& Work)
{
    FScopeLock Lock(&TasksMutex);
    for (int32 Index = Tasks.Num() - 1; Index >= 0; --Index)
    {
        if (Tasks[Index]->Complete.load(std::memory_order_acquire)) { Tasks.RemoveAtSwap(Index); }
    }
    auto Task = MakeUnique<FPostHogTask>();
    Task->Work = MoveTemp(Work);
    Task->Thread = FRunnableThread::Create(Task.Get(), TEXT("PostHogClient"));
    if (!Task->Thread) { return false; }
    Tasks.Add(MoveTemp(Task));
    return true;
}

void PostHogTasks::Drain()
{
    for (;;)
    {
        TArray<TUniquePtr<FPostHogTask>> Pending;
        { FScopeLock Lock(&TasksMutex); Swap(Pending, Tasks); }
        if (Pending.IsEmpty()) { return; }
        // The lock is released while joining: an existing task can enqueue cleanup.
        Pending.Empty();
    }
}
