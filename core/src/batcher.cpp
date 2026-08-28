#include "edgeflow/batcher.hpp"

namespace edgeflow {

Batcher::Batcher(std::size_t max_batch_size,
                  std::chrono::milliseconds max_batch_age,
                  FlushCallback on_flush)
    : max_batch_size_(max_batch_size),
      max_batch_age_(max_batch_age),
      on_flush_(std::move(on_flush)),
      batch_started_at_(std::chrono::steady_clock::now()) {}

void Batcher::add_event(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) {
        batch_started_at_ = std::chrono::steady_clock::now();
    }
    buffer_.push_back(std::move(event));
    if (should_flush_locked()) {
        flush_locked();
    }
}

void Batcher::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!buffer_.empty()) {
        flush_locked();
    }
}

bool Batcher::should_flush_locked() const {
    if (buffer_.size() >= max_batch_size_) {
        return true;
    }
    auto age = std::chrono::steady_clock::now() - batch_started_at_;
    return age >= max_batch_age_;
}

void Batcher::flush_locked() {
    std::vector<Event> batch;
    batch.swap(buffer_);
    on_flush_(std::move(batch));
}

} // namespace edgeflow
