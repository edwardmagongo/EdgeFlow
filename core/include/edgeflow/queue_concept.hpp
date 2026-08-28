#pragma once
#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>
#include "edgeflow/queue_types.hpp"

namespace edgeflow {

// The contract every EdgeFlow event queue satisfies. Constraining WorkerPool,
// Connection and Server on this turns a contract violation into a compile
// error at the point of instantiation, rather than a link error or a runtime
// surprise.
//
// pop() is blocking: it returns std::nullopt only once the queue is empty AND
// shutdown() has been called. Items already queued when shutdown() arrives are
// still drained first -- WorkerPool's `while (auto item = queue.pop())` loop
// depends on exactly that.
template <typename Q>
concept EventQueue = requires(Q queue, typename Q::value_type item) {
    typename Q::value_type;
    { queue.push(std::move(item)) } -> std::same_as<PushResult>;
    { queue.pop() } -> std::same_as<std::optional<typename Q::value_type>>;
    { queue.shutdown() } -> std::same_as<void>;
    { queue.size() } -> std::same_as<std::size_t>;
    { queue.capacity() } -> std::same_as<std::size_t>;
};

} // namespace edgeflow
