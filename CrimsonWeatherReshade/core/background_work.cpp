#include "pch.h"

#include "background_work.h"

namespace {
constexpr uint32_t kWakeEventClosed = 1u << 31;
constexpr uint32_t kWakeEventUseMask = ~kWakeEventClosed;
}

BackgroundWorkQueue::BackgroundWorkQueue(size_t capacity, bool pinWorkerModule)
    : capacity_(capacity), pinWorkerModule_(pinWorkerModule) {
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

BackgroundWorkQueue::~BackgroundWorkQueue() {
    // Teardown is explicit and must run before DllMain detach. In particular, never
    // wait for a worker from a C++ static destructor under the loader lock.
}

BackgroundSubmitResult BackgroundWorkQueue::Submit(std::string coalesceKey, Work work) {
    if (!work) {
        return BackgroundSubmitResult::StartFailed;
    }

    HMODULE failedModuleReference = nullptr;
    BackgroundSubmitResult result = BackgroundSubmitResult::Accepted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopRequested_.load()) {
            return BackgroundSubmitResult::Stopping;
        }
        if (wakeEvent_ == nullptr) {
            return BackgroundSubmitResult::StartFailed;
        }
        if (!coalesceKey.empty() && coalesceKeys_.find(coalesceKey) != coalesceKeys_.end()) {
            return BackgroundSubmitResult::Duplicate;
        }
        if (outstandingCount_ >= capacity_) {
            return BackgroundSubmitResult::Full;
        }

        bool pushedItem = false;
        try {
            pending_.push_back(WorkItem{ coalesceKey, std::move(work) });
            pushedItem = true;
            if (!coalesceKey.empty()) {
                coalesceKeys_.insert(coalesceKey);
            }
        } catch (...) {
            if (pushedItem) {
                pending_.pop_back();
            }
            return BackgroundSubmitResult::StartFailed;
        }
        ++outstandingCount_;

        if (!started_) {
            HMODULE moduleReference = nullptr;
            if (pinWorkerModule_ && !GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(&BackgroundWorkQueue::ThreadEntry),
                    &moduleReference)) {
                WorkItem failedItem = std::move(pending_.back());
                pending_.pop_back();
                if (!failedItem.coalesceKey.empty()) {
                    coalesceKeys_.erase(failedItem.coalesceKey);
                }
                --outstandingCount_;
                return BackgroundSubmitResult::StartFailed;
            }

            workerModuleReference_ = moduleReference;
            workerThread_ = CreateThread(nullptr, 0, &BackgroundWorkQueue::ThreadEntry, this, 0, nullptr);
            if (workerThread_ == nullptr) {
                failedModuleReference = workerModuleReference_;
                workerModuleReference_ = nullptr;
                WorkItem failedItem = std::move(pending_.back());
                pending_.pop_back();
                if (!failedItem.coalesceKey.empty()) {
                    coalesceKeys_.erase(failedItem.coalesceKey);
                }
                --outstandingCount_;
                result = BackgroundSubmitResult::StartFailed;
            } else {
                started_ = true;
            }
        }

        if (result == BackgroundSubmitResult::Accepted) {
            SetEvent(wakeEvent_);
        }
    }

    if (failedModuleReference) {
        FreeLibrary(failedModuleReference);
    }
    return result;
}

void BackgroundWorkQueue::SignalStopWithoutLock() noexcept {
    stopRequested_.store(true);
    SignalWakeEventWithoutLock();
}

void BackgroundWorkQueue::SignalWakeEventWithoutLock() noexcept {
    uint32_t state = wakeEventUsersAndClosed_.load();
    for (;;) {
        if ((state & kWakeEventClosed) != 0 || (state & kWakeEventUseMask) == kWakeEventUseMask) {
            return;
        }
        if (wakeEventUsersAndClosed_.compare_exchange_weak(state, state + 1)) {
            break;
        }
    }

    // CloseAfterStop marks the event closed and waits for registered signalers
    // before closing the handle, so this lock-free path is safe during DllMain.
    HANDLE event = wakeEvent_;
    if (event) {
        SetEvent(event);
    }
    wakeEventUsersAndClosed_.fetch_sub(1);
}

void BackgroundWorkQueue::StopAcceptingWork() {
    std::deque<WorkItem> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SignalStopWithoutLock();
        const size_t droppedCount = pending_.size();
        dropped.swap(pending_);
        for (const WorkItem& item : dropped) {
            if (!item.coalesceKey.empty()) {
                coalesceKeys_.erase(item.coalesceKey);
            }
        }
        outstandingCount_ = droppedCount > outstandingCount_ ? 0 : outstandingCount_ - droppedCount;
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
    }
}

bool BackgroundWorkQueue::WaitForStop(DWORD waitMilliseconds) {
    std::lock_guard<std::mutex> waitLock(waitMutex_);
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        thread = workerThread_;
    }

    if (!thread) {
        return true;
    }
    if (GetThreadId(thread) == GetCurrentThreadId()) {
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, waitMilliseconds);
    if (waitResult == WAIT_OBJECT_0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workerThread_ == thread) {
            CloseHandle(workerThread_);
            workerThread_ = nullptr;
        }
    }
    return waitResult == WAIT_OBJECT_0;
}

bool BackgroundWorkQueue::StopAndWait(DWORD waitMilliseconds) {
    StopAcceptingWork();
    return WaitForStop(waitMilliseconds);
}

bool BackgroundWorkQueue::CloseAfterStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopRequested_.load() || workerThread_ != nullptr) {
        return false;
    }
    const uint32_t previous = wakeEventUsersAndClosed_.fetch_or(kWakeEventClosed);
    if ((previous & kWakeEventClosed) != 0) {
        return wakeEvent_ == nullptr;
    }
    while ((wakeEventUsersAndClosed_.load() & kWakeEventUseMask) != 0) {
        Sleep(0);
    }
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    return true;
}

bool BackgroundWorkQueue::IsStopping() const {
    return stopRequested_.load();
}

size_t BackgroundWorkQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

size_t BackgroundWorkQueue::OutstandingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstandingCount_;
}

DWORD WINAPI BackgroundWorkQueue::ThreadEntry(void* context) {
    auto* queue = static_cast<BackgroundWorkQueue*>(context);
    queue->Run();
    queue->ReleaseWorkerModuleReference();
    return 0;
}

void BackgroundWorkQueue::Run() {
    for (;;) {
        WorkItem item;
        std::deque<WorkItem> dropped;
        bool haveWork = false;
        bool shouldStop = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_.load()) {
                dropped.swap(pending_);
                outstandingCount_ = active_ ? 1 : 0;
                coalesceKeys_.clear();
                shouldStop = true;
            } else if (!pending_.empty()) {
                item = std::move(pending_.front());
                pending_.pop_front();
                active_ = true;
                haveWork = true;
            }
        }
        if (shouldStop) {
            return;
        }

        if (!haveWork) {
            WaitForSingleObject(wakeEvent_, INFINITE);
            continue;
        }

        try {
            item.work(stopRequested_);
        } catch (...) {
            // A failed job must not terminate the shared worker or prevent queued jobs from running.
        }

        std::string completedKey = std::move(item.coalesceKey);
        item.work = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!completedKey.empty()) {
                coalesceKeys_.erase(completedKey);
            }
            if (outstandingCount_ > 0) {
                --outstandingCount_;
            }
            active_ = false;
        }
    }
}

void BackgroundWorkQueue::ReleaseWorkerModuleReference() {
    HMODULE moduleReference = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        moduleReference = workerModuleReference_;
        workerModuleReference_ = nullptr;
        started_ = false;
    }

    if (moduleReference) {
        FreeLibraryAndExitThread(moduleReference, 0);
    }
    ExitThread(0);
}

OwnerCompletionQueue::OwnerCompletionQueue(size_t capacity)
    : capacity_(capacity) {
}

bool OwnerCompletionQueue::Post(uint64_t generation, std::function<void()> completion) {
    if (!completion) {
        return false;
    }
    return TryPost(generation, completion);
}

bool OwnerCompletionQueue::PostUntilAccepted(
    uint64_t generation,
    std::function<void()> completion,
    const std::atomic<bool>& stopRequested) {
    if (!completion) {
        return false;
    }
    while (!stopRequested.load()) {
        if (TryPost(generation, completion)) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool OwnerCompletionQueue::TryPost(uint64_t generation, std::function<void()>& completion) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.size() >= capacity_) return false;
        pending_.push_back(CompletionItem{ generation, std::move(completion) });
        return true;
    } catch (...) {
        return false;
    }
}

size_t OwnerCompletionQueue::Drain(uint64_t currentGeneration, size_t maximum) {
    const DWORD threadId = GetCurrentThreadId();
    std::lock_guard<std::mutex> drainLock(drainMutex_);
    ownerThreadId_.store(threadId);

    size_t drained = 0;
    while (drained < maximum) {
        CompletionItem item{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty()) {
                break;
            }
            item = std::move(pending_.front());
            pending_.pop_front();
        }
        if (item.generation == currentGeneration) {
            try {
                item.completion();
            } catch (...) {
                // A failed completion must not block the rest of the owner-thread queue.
            }
        }
        ++drained;
    }
    return drained;
}

size_t OwnerCompletionQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

void OwnerCompletionQueue::Clear() {
    std::deque<CompletionItem> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped.swap(pending_);
    }
}

DWORD OwnerCompletionQueue::OwnerThreadId() const {
    return ownerThreadId_.load();
}
