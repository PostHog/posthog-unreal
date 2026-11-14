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
    TArray<TUniquePtr<FPostHogTask>> Reaped;
    auto Task = MakeUnique<FPostHogTask>();
    Task->Work = MoveTemp(Work);
    bool bStarted = false;
    {
        FScopeLock Lock(&TasksMutex);
        for (int32 Index = Tasks.Num() - 1; Index >= 0; --Index)
        {
            if (Tasks[Index]->Complete.load(std::memory_order_acquire))
            { Reaped.Add(MoveTemp(Tasks[Index])); Tasks.RemoveAtSwap(Index); }
        }
        Task->Thread = FRunnableThread::Create(Task.Get(), TEXT("PostHogClient"));
        bStarted = Task->Thread != nullptr;
        if (bStarted) { Tasks.Add(MoveTemp(Task)); }
    }
    // Failed work can own application captures with reentrant destructors.
    // Release those captures, and join completed threads, outside TasksMutex.
    return bStarted;
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
