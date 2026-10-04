#include "core/background_work.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

bool WaitUntil(const std::atomic<bool>& value, DWORD timeoutMs = 5000) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    while (!value.load() && GetTickCount64() < end) {
        Sleep(1);
    }
    return value.load();
}

struct ReentrantCapture {
    BackgroundWorkQueue* queue = nullptr;
    std::atomic<bool>* destroyed = nullptr;
    ~ReentrantCapture() {
        if (queue && destroyed) {
            (void)queue->PendingCount();
            destroyed->store(true);
        }
    }
};

void TestCapacityCoalescingAndStopTimeout() {
    BackgroundWorkQueue queue(1, false);
    std::atomic<bool> entered{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> stopObserved{ false };

    Require(queue.Submit("refresh", [&](const std::atomic<bool>& stop) {
        entered.store(true);
        while (!release.load()) {
            if (stop.load()) stopObserved.store(true);
            Sleep(1);
        }
    }) == BackgroundSubmitResult::Accepted, "first bounded job is accepted");
    Require(WaitUntil(entered), "worker started first job");
    Require(queue.Submit("refresh", [](const std::atomic<bool>&) {}) == BackgroundSubmitResult::Duplicate,
        "duplicate keyed job is coalesced");
    Require(queue.Submit("other", [](const std::atomic<bool>&) {}) == BackgroundSubmitResult::Full,
        "queue capacity rejects excess work");

    queue.StopAcceptingWork();
    Require(WaitUntil(stopObserved), "active job observes cooperative cancellation");
    Require(!queue.WaitForStop(1), "timed out worker wait reports incomplete without closing handle");
    Require(queue.IsStopping(), "stop state remains visible after a timed out wait");
    release.store(true);
    Require(queue.WaitForStop(5000), "retained worker handle permits a later successful wait");
}

void TestDroppedCaptureDestructorsRunOutsideQueueLock() {
    BackgroundWorkQueue queue(2, false);
    std::atomic<bool> entered{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> captureDestroyed{ false };
    Require(queue.Submit("active", [&](const std::atomic<bool>&) {
        entered.store(true);
        while (!release.load()) Sleep(1);
    }) == BackgroundSubmitResult::Accepted, "active work accepted for lock test");
    Require(WaitUntil(entered), "active lock-test work started");

    auto capture = std::make_shared<ReentrantCapture>();
    capture->queue = &queue;
    capture->destroyed = &captureDestroyed;
    Require(queue.Submit("dropped", [capture](const std::atomic<bool>&) {}) == BackgroundSubmitResult::Accepted,
        "pending work accepted for cancellation");
    capture.reset();

    queue.StopAcceptingWork();
    Require(WaitUntil(captureDestroyed), "dropping pending work destroys captures outside queue lock");
    release.store(true);
    Require(queue.WaitForStop(5000), "stopped queue worker exits after active work returns");
}

void TestExceptionsDoNotKillWorker() {
    BackgroundWorkQueue queue(3, false);
    std::atomic<bool> secondRan{ false };
    Require(queue.Submit("throws", [](const std::atomic<bool>&) { throw 7; }) == BackgroundSubmitResult::Accepted,
        "throwing job accepted");
    Require(queue.Submit("continues", [&](const std::atomic<bool>&) { secondRan.store(true); }) == BackgroundSubmitResult::Accepted,
        "job after throwing job accepted");
    Require(WaitUntil(secondRan), "worker contains exception and continues");
    Require(queue.StopAndWait(5000), "exception-test worker stops cleanly");
}

void TestOwnerCompletionGenerationAndThread() {
    OwnerCompletionQueue completions(2);
    std::atomic<DWORD> appliedThread{ 0 };
    std::atomic<int> applied{ 0 };
    Require(completions.Post(3, [&] { applied.fetch_add(100); }), "stale completion posts");
    Require(completions.Post(4, [&] {
        appliedThread.store(GetCurrentThreadId());
        applied.fetch_add(1);
    }), "current completion posts");
    Require(!completions.Post(4, [] {}), "completion queue rejects work at its configured bound");
    std::atomic<size_t> migratedDrain{ 0 };
    std::thread migratedOwner([&] { migratedDrain.store(completions.Drain(4)); });
    const DWORD migratedThread = GetThreadId(migratedOwner.native_handle());
    migratedOwner.join();
    Require(migratedDrain.load() == 2, "first available callback owner drains current and stale completions");
    Require(applied.load() == 1, "stale generation is discarded");
    Require(appliedThread.load() == migratedThread, "completion executes on the current serialized callback thread");

    Require(completions.Post(4, [] {}), "first critical completion fills pending queue");
    Require(completions.Post(4, [] {}), "second critical completion fills pending queue");
    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> acceptedAfterWait{ false };
    std::thread waitingProducer([&] {
        acceptedAfterWait.store(completions.PostUntilAccepted(4, [] {}, stopRequested));
    });
    Sleep(10);
    Require(!acceptedAfterWait.load(), "critical completion waits while the bounded queue is full");
    stopRequested.store(true);
    waitingProducer.join();
    Require(!acceptedAfterWait.load(), "critical completion wait exits when shutdown cancels it");
    Require(completions.Drain(4) == 2, "owner callback clears completions after cancelled post");

    Require(completions.Post(4, [] {}), "queue accepts work after the cancelled producer exits");
    Require(completions.Post(4, [] {}), "queue fills before successful wait test");
    stopRequested.store(false);
    std::atomic<bool> acceptedAfterDrain{ false };
    std::thread drainWaitingProducer([&] {
        acceptedAfterDrain.store(completions.PostUntilAccepted(4, [] {}, stopRequested));
    });
    Sleep(10);
    Require(completions.Drain(4) == 2, "owner callback frees bounded completion capacity");
    drainWaitingProducer.join();
    Require(acceptedAfterDrain.load(), "critical completion is accepted after owner drain frees capacity");
    Require(completions.Drain(4) == 1, "owner callback applies the accepted critical completion");
}

} // namespace

int main() {
    TestCapacityCoalescingAndStopTimeout();
    TestDroppedCaptureDestructorsRunOutsideQueueLock();
    TestExceptionsDoNotKillWorker();
    TestOwnerCompletionGenerationAndThread();
    std::puts("Background work queue harness passed.");
    return 0;
}
