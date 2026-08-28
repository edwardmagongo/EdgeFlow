#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "edgeflow/queue_types.hpp"

namespace edgeflow {

// A bounded lock-free multi-producer/multi-consumer queue: Dmitry Vyukov's
// ring buffer, where each slot carries its own atomic sequence counter and
// producers/consumers claim positions with a CAS on a shared cursor. No mutex
// is taken on the push path while consumers are awake.
//
// Deliberate differences from BoundedQueue, each covered by tests:
//
//   * Capacity is rounded UP to a power of two, with a floor of two slots, so
//     the ring can index with a mask instead of a modulo. capacity() reports
//     the rounded value, so LockFreeBoundedQueue(50, ...) really does hold 64
//     items and LockFreeBoundedQueue(1, ...) holds 2. The floor is a hard
//     requirement of the algorithm, not a tuning choice -- see
//     validated_capacity().
//
//   * BackpressurePolicy::DropOldest is APPROXIMATE. Making room means
//     discarding from the head, which is a consumer-side operation being
//     performed by a producer. With a single producer -- which is what the
//     gateway has, its io_context being single-threaded -- the discard-then-
//     retry always succeeds and behaviour matches BoundedQueue exactly. With
//     several concurrent producers another producer may claim the freed slot
//     first, in which case push() reports RejectedBackpressure rather than
//     DroppedOldest, and the item discarded is not necessarily the oldest.
//
//   * size() is approximate under concurrent access (it reads two cursors that
//     can move between the reads) and exact when the queue is quiescent.
//
//   * T must be default-constructible, because the ring preallocates all slots
//     at construction. BoundedQueue has no such requirement.
//
// THE BLOCKING pop(). pop() must block, because WorkerPool's drain loop depends
// on it, and blocking means parking a thread -- something a strictly lock-free
// structure cannot do. The resolution is a hybrid:
//
//   1. A bounded spin on the lock-free fast path, which covers the common case
//      where an item is a few nanoseconds away.
//   2. Failing that, register in waiters_ and park on a condition variable.
//
// A producer touches wait_mutex_ and not_empty_ ONLY when waiters_ is non-zero.
// Under load -- queue non-empty, workers busy -- waiters_ stays 0 and no
// producer ever takes a lock. The mutex appears only once a consumer has
// actually gone to sleep.
//
// This is NOT a claim that the whole structure is lock-free. The fast path is;
// the empty-queue wait is not. The distinction is stated rather than oversold.
template <typename T>
class LockFreeBoundedQueue {
    static_assert(std::is_default_constructible_v<T>,
                  "LockFreeBoundedQueue preallocates its ring, so T must be "
                  "default-constructible");

public:
    using value_type = T;

    LockFreeBoundedQueue(std::size_t capacity, BackpressurePolicy policy)
        : buffer_(validated_capacity(capacity)),
          mask_(buffer_.size() - 1),
          policy_(policy) {
        for (std::size_t i = 0; i < buffer_.size(); ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeBoundedQueue(const LockFreeBoundedQueue&) = delete;
    LockFreeBoundedQueue& operator=(const LockFreeBoundedQueue&) = delete;

    // Non-blocking. Applies the configured backpressure policy when full.
    PushResult push(T item) {
        if (try_push(item)) {
            notify_one_waiter();
            return PushResult::Accepted;
        }
        if (policy_ == BackpressurePolicy::DropOldest) {
            if (try_pop().has_value() && try_push(item)) {
                notify_one_waiter();
                return PushResult::DroppedOldest;
            }
            // Lost the freed slot to another producer; see the class comment.
            return PushResult::RejectedBackpressure;
        }
        return PushResult::RejectedBackpressure;
    }

    // Blocking pop. Returns std::nullopt only once the queue is empty AND
    // shutdown() has been called; items queued before shutdown are drained
    // first.
    std::optional<T> pop() {
        // Phase 1: bounded spin. Covers the case where an item is imminent,
        // without paying for a mutex or a context switch.
        for (unsigned attempt = 0; attempt < kSpinAttempts; ++attempt) {
            if (auto item = try_pop()) {
                return item;
            }
            if (shutting_down_.load(std::memory_order_acquire)) {
                return try_pop();
            }
            std::this_thread::yield();
        }

        // Phase 2: park. The lock is taken once and held across iterations
        // (wait() releases it while sleeping), because re-locking an already-
        // held std::mutex on the next pass would deadlock.
        std::unique_lock<std::mutex> lock(wait_mutex_);
        while (true) {
            waiters_.fetch_add(1, std::memory_order_relaxed);
            // Publishes the registration before the re-check below reads the
            // ring. Pairs with the fence in notify_one_waiter(); see the class
            // comment on why release/acquire alone would lose wakeups.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Re-check AFTER registering. A producer that pushed between the
            // spin phase and this registration would have seen waiters_ == 0
            // and skipped its notify, so without this we would sleep on an item
            // that is already sitting in the ring.
            auto item = try_pop();
            const bool draining = shutting_down_.load(std::memory_order_acquire);

            if (!item && !draining) {
                not_empty_.wait(lock);
                waiters_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            waiters_.fetch_sub(1, std::memory_order_relaxed);
            if (item) {
                return item;
            }
            // Shutting down: one last attempt, so an item that landed between
            // the try_pop above and here is still delivered rather than lost.
            return try_pop();
        }
    }

    void shutdown() {
        shutting_down_.store(true, std::memory_order_release);
        // Taken even though the flag is atomic: a consumer that has read the
        // flag as false but not yet called wait() is holding this mutex, so
        // acquiring it here guarantees the notify_all lands after that consumer
        // is actually waiting rather than before.
        std::lock_guard<std::mutex> lock(wait_mutex_);
        not_empty_.notify_all();
    }

    // Approximate under concurrency, exact when quiescent.
    std::size_t size() const {
        std::size_t enqueued = enqueue_pos_.load(std::memory_order_acquire);
        std::size_t dequeued = dequeue_pos_.load(std::memory_order_acquire);
        return enqueued > dequeued ? enqueued - dequeued : 0;
    }

    std::size_t capacity() const { return mask_ + 1; }

private:
    // One ring slot. `sequence` encodes whose turn it is: a producer may write
    // when sequence == its claimed position, a consumer may read when
    // sequence == position + 1.
    struct Slot {
        std::atomic<std::size_t> sequence{0};
        T storage{};
    };

    // Keeps the ring well under half of size_t so the signed difference used
    // below cannot overflow in any realistic run.
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;
    static constexpr std::size_t kCacheLineBytes = 64;
    // Long enough to ride out the microsecond-scale gaps between a worker
    // finishing an event and the next one arriving; short enough that a
    // genuinely idle worker reaches the parking path promptly.
    static constexpr unsigned kSpinAttempts = 64;

    static std::size_t validated_capacity(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("capacity must be greater than 0");
        }
        if (capacity > kMaxCapacity) {
            throw std::invalid_argument("capacity exceeds maximum of 1073741824");
        }
        std::size_t rounded = 1;
        while (rounded < capacity) {
            rounded <<= 1;
        }
        // The ring needs at least two slots. With a single slot the mask is 0,
        // so a written-but-unconsumed slot (sequence == pos + 1) and a slot
        // freed for the producer one lap ahead (sequence == pos + mask + 1)
        // carry the SAME sequence value. A producer then cannot tell full from
        // free: it overwrites the live item, drives enqueue_pos_ past
        // dequeue_pos_, and leaves try_pop() spinning on a cursor that never
        // moves. Rounding 1 up to 2 is the same kind of documented capacity
        // adjustment as rounding 50 up to 64.
        return rounded < 2 ? 2 : rounded;
    }

    // Wakes one parked consumer, if any. The waiters_ check is what keeps the
    // hot path lock-free: with every worker busy this reads one atomic and
    // returns, never touching the mutex.
    void notify_one_waiter() {
        // Orders the push's publication before this read of waiters_. Pairs
        // with the fence in pop(). Without BOTH fences the producer may read a
        // stale waiters_ == 0 while the consumer reads a stale empty ring, and
        // the wakeup is lost.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters_.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(wait_mutex_);
        not_empty_.notify_one();
    }

    bool try_push(T& item) {
        Slot* slot = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        while (true) {
            slot = &buffer_[pos & mask_];
            std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                // The slot is free and it is our turn; try to claim the position.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // the ring is full
            } else {
                // Another producer moved the cursor; re-read and retry.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        slot->storage = std::move(item);
        // Release: publishes the stored value to whichever consumer reads this
        // sequence with acquire.
        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() {
        Slot* slot = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        while (true) {
            slot = &buffer_[pos & mask_];
            std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
            auto diff =
                static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt; // the ring is empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        T item = std::move(slot->storage);
        // Hand the slot to the producer one lap ahead.
        slot->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return item;
    }

    std::vector<Slot> buffer_;
    std::size_t mask_;
    BackpressurePolicy policy_;
    // The two cursors sit on separate cache lines: producers hammer one and
    // consumers the other, and sharing a line would reintroduce exactly the
    // contention this queue exists to avoid.
    alignas(kCacheLineBytes) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineBytes) std::atomic<std::size_t> dequeue_pos_{0};
    std::atomic<bool> shutting_down_{false};
    // The parking apparatus. Touched by a producer only when waiters_ > 0.
    std::atomic<std::size_t> waiters_{0};
    std::mutex wait_mutex_;
    std::condition_variable not_empty_;
};

} // namespace edgeflow
