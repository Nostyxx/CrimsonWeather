#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

#include <Windows.h>

enum class BackgroundSubmitResult {
    Accepted,
    Duplicate,
    Full,
    Stopping,
    StartFailed,
};

class BackgroundWorkQueue {
public:
    using Work = std::function<void(const std::atomic<bool>& stopRequested)>;

    explicit BackgroundWorkQueue(size_t capacity, bool pinWorkerModule = true);
    ~BackgroundWorkQueue();
    BackgroundWorkQueue(const BackgroundWorkQueue&) = delete;
    BackgroundWorkQueue& operator=(const BackgroundWorkQueue&) = delete;

    BackgroundSubmitResult Submit(std::string coalesceKey, Work work);
    void SignalStopWithoutLock() noexcept;
    void StopAcceptingWork();
    bool WaitForStop(DWORD waitMilliseconds);
    bool StopAndWait(DWORD waitMilliseconds);
    bool CloseAfterStop();
    bool IsStopping() const;
    size_t PendingCount() const;
    size_t OutstandingCount() const;

private:
    struct WorkItem {
        std::string coalesceKey;
        Work work;
    };

    static DWORD WINAPI ThreadEntry(void* context);
    void Run();
    void ReleaseWorkerModuleReference();
    void SignalWakeEventWithoutLock() noexcept;

    const size_t capacity_;
    const bool pinWorkerModule_;
    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::deque<WorkItem> pending_;
    std::unordered_set<std::string> coalesceKeys_;
    size_t outstandingCount_ = 0;
    bool active_ = false;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<uint32_t> wakeEventUsersAndClosed_{ 0 };
    HANDLE workerThread_ = nullptr;
    HMODULE workerModuleReference_ = nullptr;
    std::atomic<bool> stopRequested_{ false };
    bool started_ = false;
};

class OwnerCompletionQueue {
public:
    explicit OwnerCompletionQueue(size_t capacity = 128);
    bool Post(uint64_t generation, std::function<void()> completion);
    bool PostUntilAccepted(
        uint64_t generation,
        std::function<void()> completion,
        const std::atomic<bool>& stopRequested);
    size_t Drain(uint64_t currentGeneration, size_t maximum = 64);
    size_t PendingCount() const;
    DWORD OwnerThreadId() const;
    void Clear();

private:
    struct CompletionItem {
        uint64_t generation;
        std::function<void()> completion;
    };

    mutable std::mutex mutex_;
    std::mutex drainMutex_;
    std::deque<CompletionItem> pending_;
    std::atomic<DWORD> ownerThreadId_{ 0 };
    const size_t capacity_;

    bool TryPost(uint64_t generation, std::function<void()>& completion);
};
