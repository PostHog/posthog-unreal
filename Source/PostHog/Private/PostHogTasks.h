#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"

/** Tracks native-only work and joins its threads before module unload. */
namespace PostHogTasks
{
    bool Launch(TFunction<void()>&& Work);
    void Drain();

    template <typename FunctionType>
    auto Run(FunctionType&& Function) -> TSharedFuture<decltype(Function())>
    {
        using ResultType = decltype(Function());
        auto Promise = MakeShared<TPromise<ResultType>, ESPMode::ThreadSafe>();
        auto Future = Promise->GetFuture().Share();
        if (!Launch([Promise, Function = Forward<FunctionType>(Function)]() mutable
        {
            Promise->SetValue(Function());
        }))
        {
            Promise->SetValue(ResultType());
        }
        return Future;
    }
}
