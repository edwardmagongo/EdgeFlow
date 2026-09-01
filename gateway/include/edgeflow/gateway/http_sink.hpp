#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"
#include "edgeflow/queue_types.hpp"
#include "edgeflow/sink.hpp"
#include "edgeflow/stats.hpp"

namespace edgeflow::gateway {

// A Sink that POSTs each batch to an HTTP endpoint as NDJSON.
//
// consume() does NO network I/O. It serialises the batch and pushes it onto the
// OUTBOUND QUEUE, then returns. N dedicated threads (Options::concurrency) drain
// that queue and do the round trips. Worker threads therefore never block on
// the backend, which keeps queue-wait latency a measure of EdgeFlow rather than
// of the backend -- the same reason Phase 1 put Batcher::flush() on its own
// thread instead of the reactor.
//
// Concurrency is safe here because each batch carries its own idempotency key,
// minted once in consume() (see next_idempotency_key). Two threads sending two
// batches never contend, and a RETRY of one batch stays inside that batch's own
// send_with_retries call on one thread. What concurrency does change is ARRIVAL
// ORDER: batches may commit out of creation order, so row id order no longer
// tracks batch order. The query API sorts by (timestamp, id) on device-supplied
// timestamps, which this does not affect.
//
// Two queues now exist in the gateway. This one is the OUTBOUND QUEUE; the one
// carrying TimedEvents into the worker pool is the EVENT QUEUE.
//
// The body is byte-identical to what FileSink writes for the same batch, so a
// file can be replayed to the backend with curl.
//
// NOTE on BackpressurePolicy::Block: under Block, a full outbound queue makes
// consume() wait, which stalls a worker and couples backend latency back into
// the pipeline -- exactly what the sink thread exists to prevent. It is offered
// because an operator who would rather stall than lose data is making a real
// trade, but it is not the default, and the non-blocking guarantee above holds
// for the drop-* policies only.
class HttpSink : public edgeflow::Sink {
public:
    struct Options {
        std::string url;                                    // http://host:port/path
        std::size_t outbound_capacity = 256;                // in batches
        std::size_t concurrency = 4;                        // sink threads draining the queue
        edgeflow::BackpressurePolicy backpressure =
            edgeflow::BackpressurePolicy::DropOldest;
        std::size_t max_retries = 3;                        // attempts AFTER the first
        std::chrono::milliseconds backoff_base{100};        // doubles per attempt
        std::chrono::milliseconds timeout{5000};            // connect and request
    };

    HttpSink(Options options, edgeflow::Stats& stats);
    ~HttpSink() override;

    HttpSink(const HttpSink&) = delete;
    HttpSink& operator=(const HttpSink&) = delete;

    void consume(const std::vector<edgeflow::Event>& batch) override;

    // Drains the outbound queue, then joins the sink threads. Idempotent, and
    // called by the destructor.
    void stop();

private:
    void run();                                  // sink thread body
    // What a single round trip tells us to do next.
    enum class SendOutcome {
        Success,           // 2xx
        RetryableFailure,  // transport error, 5xx, or 429 -- may clear on its own
        PermanentFailure,  // 4xx other than 429, or 3xx -- will not improve
    };

    // One queued batch: the serialised body together with the idempotency key
    // minted for it. The key lives WITH the batch rather than being generated
    // at send time, which is precisely what makes it survive a retry.
    struct OutboundBatch {
        std::string body;
        std::string idempotency_key;
    };

    SendOutcome send_once(const OutboundBatch& batch);
    // Runs send_once up to 1 + options_.max_retries times, backing off between
    // attempts. Returns true if the batch was delivered.
    bool send_with_retries(const OutboundBatch& batch);

    // A random per-process prefix plus a monotonic counter. Unique within the
    // process by construction, and unique across processes with overwhelming
    // probability.
    //
    // Thread-safe because consume() runs on whichever thread flushed the
    // Batcher -- worker threads and the periodic flush thread both do. Batcher
    // happens to call the sink callback under its own mutex today, but that is
    // Batcher's internal detail rather than part of the Sink contract, so this
    // does not lean on it. Note this deliberately does NOT use the jitter
    // generator in send_with_retries, which is thread_local and only ever
    // touched by sink threads.
    std::string next_idempotency_key();
    static std::string make_key_prefix();

    // Split from the URL once at construction so the send path does no parsing.
    struct Target {
        std::string host;
        std::string port;
        std::string path;
    };
    static Target parse_url(const std::string& url);

    Options options_;
    Target target_;
    edgeflow::Stats& stats_;
    edgeflow::BoundedQueue<OutboundBatch> outbound_;
    const std::string key_prefix_ = make_key_prefix();
    std::atomic<std::uint64_t> key_counter_{0};
    // How long stop() will spend draining before abandoning the backlog. Long
    // enough for a healthy backend to finish, short enough that an unreachable
    // one cannot hang gateway shutdown.
    static constexpr std::chrono::seconds kDrainDeadline{5};
    std::atomic<bool> draining_{false};
    std::chrono::steady_clock::time_point drain_deadline_{};
    std::vector<std::thread> threads_;
    bool stopped_ = false;
};

} // namespace edgeflow::gateway
