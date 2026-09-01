#include <boost/asio.hpp>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include "edgeflow/batcher.hpp"
#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/config.hpp"
#include "edgeflow/gateway/http_sink.hpp"
#include "edgeflow/gateway/server.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"
#include "edgeflow/worker_pool.hpp"

// Which queue this binary uses is fixed at compile time: edgeflow-gateway builds
// with the mutex queue, edgeflow-gateway-lockfree with EDGEFLOW_USE_LOCK_FREE_QUEUE
// defined. Compile-time rather than a runtime virtual interface, because a virtual
// call on the push path (~2-5ns against a ~19-30ns push) would distort the exact
// quantity these two binaries exist to compare.
#if defined(EDGEFLOW_USE_LOCK_FREE_QUEUE)
#include "edgeflow/lock_free_bounded_queue.hpp"
using GatewayQueue = edgeflow::LockFreeBoundedQueue<edgeflow::TimedEvent>;
inline constexpr const char* kQueueName = "lock-free";
#else
#include "edgeflow/bounded_queue.hpp"
using GatewayQueue = edgeflow::BoundedQueue<edgeflow::TimedEvent>;
inline constexpr const char* kQueueName = "mutex";
#endif

int main(int argc, char** argv) {
    edgeflow::gateway::Config config;
    try {
        config = edgeflow::gateway::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }

    try {
        GatewayQueue queue(config.queue_capacity, config.backpressure);
        // Stats is declared before the sink because HttpSink holds a reference
        // to it.
        edgeflow::Stats stats;

        // Runtime selection is fine here in a way it was not for the queue in
        // Phase 3: Sink is already virtual, and this is one dispatch per BATCH,
        // not per event.
        std::unique_ptr<edgeflow::Sink> sink;
        if (config.sink_kind == edgeflow::gateway::SinkKind::Http) {
            edgeflow::gateway::HttpSink::Options sink_options;
            sink_options.url = config.sink_url;
            sink_options.outbound_capacity = config.sink_outbound_capacity;
            sink_options.concurrency = config.sink_concurrency;
            sink_options.backpressure = config.sink_backpressure;
            sink_options.max_retries = config.sink_max_retries;
            sink_options.backoff_base = config.sink_backoff_ms;
            sink_options.timeout = config.sink_timeout_ms;
            sink = std::make_unique<edgeflow::gateway::HttpSink>(std::move(sink_options), stats);
        } else {
            sink = std::make_unique<edgeflow::FileSink>(config.sink_file);
        }

        edgeflow::Batcher batcher(config.batch_size, config.batch_age,
                                    [&sink](std::vector<edgeflow::Event> batch) { sink->consume(batch); });
        edgeflow::WorkerPool pool(queue, batcher, stats, config.workers);

        boost::asio::io_context io_context;

        // Register the signal set before starting worker threads so a SIGINT that
        // arrives early is still handled gracefully via io_context.run() rather than
        // taking the default terminate action while workers are live.
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&io_context](const boost::system::error_code&, int) {
            io_context.stop();
        });

        pool.start();

        edgeflow::gateway::Server server(io_context, config.port, queue, config.backpressure, stats);

        // Periodic flush thread: Batcher only checks its age threshold inside
        // add_event(), so without this an idle gateway (no new events arriving)
        // could leave a partial batch unflushed indefinitely. This runs on a
        // dedicated thread -- NOT on the io_context -- because Batcher::flush()
        // does blocking disk I/O (via FileSink) and takes Batcher's mutex; doing
        // that from an io_context handler would block the single network I/O
        // thread that also services accepts, reads, and backpressure-retry
        // timers, which is exactly the blocking-on-the-reactor-thread problem
        // the Block backpressure policy's non-blocking retry timers exist to
        // avoid. The thread wakes on its own schedule (independent of event
        // arrivals) to keep --batch-age-ms an actual latency bound,
        // supplementing (not replacing) the in-add_event checks.
        std::mutex flush_shutdown_mutex;
        std::condition_variable flush_shutdown_cv;
        bool flush_shutdown = false;
        std::thread flush_thread([&]() {
            std::unique_lock<std::mutex> lock(flush_shutdown_mutex);
            while (!flush_shutdown) {
                if (flush_shutdown_cv.wait_for(lock, config.batch_age,
                                                [&] { return flush_shutdown; })) {
                    // Woken by the shutdown notification: exit without flushing.
                    // The post-io_context.run() shutdown sequence below already
                    // performs the final flush.
                    break;
                }
                // Timed out with no shutdown signaled: do the periodic flush.
                // Batcher::flush() takes its own internal mutex, so it must not
                // be called while still holding flush_shutdown_mutex.
                lock.unlock();
                batcher.flush();
                lock.lock();
            }
        });

        // queue.capacity() rather than config.queue_capacity: the lock-free queue
        // rounds up to a power of two with a floor of 2, so the requested and actual
        // capacities can differ. A benchmark comparison has to report what the queue
        // actually holds, not what was asked for.
        const char* sink_name =
            config.sink_kind == edgeflow::gateway::SinkKind::Http ? "http" : "file";
        // Flushed explicitly (not just "\n"): stdout is fully buffered rather
        // than line-buffered once it isn't a TTY, which is exactly the case
        // when a parent process spawns this binary with a piped stdout. Without
        // an explicit flush this line sits in libc's buffer until the process
        // exits, so anything (e.g. a test harness) trying to use this line as a
        // "the gateway is now accepting connections" signal would never see it
        // until shutdown -- long after the signal was needed.
        std::cout << "edgeflow-gateway listening on port " << config.port
                  << " (queue=" << kQueueName
                  << ", workers=" << config.workers
                  << ", queue_capacity=" << queue.capacity()
                  << ", sink=" << sink_name << ")" << std::endl;

        io_context.run();

        {
            std::lock_guard<std::mutex> lock(flush_shutdown_mutex);
            flush_shutdown = true;
        }
        flush_shutdown_cv.notify_one();
        flush_thread.join();

        pool.stop();
        batcher.flush();
        auto snapshot = stats.snapshot();
        std::cout << "edgeflow-gateway shut down (accepted=" << snapshot.events_accepted
                  << ", dropped_oldest=" << snapshot.events_dropped_oldest
                  << ", dropped_newest=" << snapshot.events_dropped_newest
                  << ", malformed=" << snapshot.events_malformed
                  << ", queue_wait_count=" << snapshot.queue_wait_count
                  << ", queue_wait_mean_us=" << snapshot.queue_wait_mean_us
                  << ", queue_wait_p50_us=" << snapshot.queue_wait_p50_us
                  << ", queue_wait_p99_us=" << snapshot.queue_wait_p99_us
                  // Appended AFTER every existing key. run_benchmarks.py's
                  // SHUTDOWN_RE (which run_saturation_sweep.py imports) is
                  // anchored on the order above; adding keys at the end is safe,
                  // reordering or renaming is not.
                  << ", batches_sent=" << snapshot.batches_sent
                  << ", batches_retried=" << snapshot.batches_retried
                  << ", batches_dropped_outbound=" << snapshot.batches_dropped_outbound
                  << ", batches_dropped_exhausted=" << snapshot.batches_dropped_exhausted
                  << ")\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }
}
