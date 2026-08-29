#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
// OUTBOUND QUEUE, then returns. A dedicated thread drains that queue and does
// the round trips. Worker threads therefore never block on the backend, which
// keeps queue-wait latency a measure of EdgeFlow rather than of the backend --
// the same reason Phase 1 put Batcher::flush() on its own thread instead of the
// reactor.
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

    // Drains the outbound queue, then joins the sink thread. Idempotent, and
    // called by the destructor.
    void stop();

private:
    void run();                                  // sink thread body
    bool send_once(const std::string& body);     // one round trip; true on 2xx

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
    edgeflow::BoundedQueue<std::string> outbound_;
    std::thread thread_;
    bool stopped_ = false;
};

} // namespace edgeflow::gateway
