#pragma once

namespace edgeflow {

// What a queue does when push() arrives and the queue is full.
enum class BackpressurePolicy { Block, DropOldest, DropNewest };

// The outcome of a push().
enum class PushResult { Accepted, DroppedOldest, RejectedBackpressure };

} // namespace edgeflow
