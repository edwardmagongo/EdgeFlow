#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace edgeflow {

enum class BackpressurePolicy { Block, DropOldest, DropNewest };

enum class PushResult { Accepted, DroppedOldest, RejectedBackpressure };

template <typename T>
class BoundedQueue {
public:
    BoundedQueue(std::size_t capacity, BackpressurePolicy policy)
        : capacity_(capacity), policy_(policy) {}

    // Non-blocking. Applies the configured backpressure policy when full.
    PushResult push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.size() < capacity_) {
            items_.push_back(std::move(item));
            not_empty_.notify_one();
            return PushResult::Accepted;
        }

        if (policy_ == BackpressurePolicy::DropOldest) {
            items_.pop_front();
            items_.push_back(std::move(item));
            not_empty_.notify_one();
            return PushResult::DroppedOldest;
        }

        return PushResult::RejectedBackpressure;
    }

    // Blocking pop, for worker threads. Returns std::nullopt only after shutdown().
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !items_.empty() || shutting_down_; });
        if (items_.empty()) {
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        not_empty_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }

private:
    std::size_t capacity_;
    BackpressurePolicy policy_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool shutting_down_ = false;
};

} // namespace edgeflow
