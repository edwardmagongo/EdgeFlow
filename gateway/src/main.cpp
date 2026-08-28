#include <boost/asio.hpp>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/config.hpp"
#include "edgeflow/gateway/server.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"
#include "edgeflow/worker_pool.hpp"

int main(int argc, char** argv) {
    edgeflow::gateway::Config config;
    try {
        config = edgeflow::gateway::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }

    try {
        edgeflow::BoundedQueue<edgeflow::TimedEvent> queue(config.queue_capacity, config.backpressure);
        edgeflow::FileSink sink(config.sink_file);
        edgeflow::Batcher batcher(config.batch_size, config.batch_age,
                                    [&sink](std::vector<edgeflow::Event> batch) { sink.consume(batch); });
        edgeflow::Stats stats;
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

        std::cout << "edgeflow-gateway listening on port " << config.port
                  << " (workers=" << config.workers
                  << ", queue_capacity=" << config.queue_capacity << ")\n";

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
                  << ", queue_wait_p99_us=" << snapshot.queue_wait_p99_us << ")\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }
}
