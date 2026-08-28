#include <boost/asio.hpp>
#include <iostream>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/config.hpp"
#include "edgeflow/worker_pool.hpp"
#include "server.hpp"

int main(int argc, char** argv) {
    edgeflow::gateway::Config config;
    try {
        config = edgeflow::gateway::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }

    try {
        edgeflow::BoundedQueue<edgeflow::Event> queue(config.queue_capacity, config.backpressure);
        edgeflow::FileSink sink(config.sink_file);
        edgeflow::Batcher batcher(config.batch_size, config.batch_age,
                                    [&sink](std::vector<edgeflow::Event> batch) { sink.consume(batch); });
        edgeflow::WorkerPool pool(queue, batcher, config.workers);

        boost::asio::io_context io_context;

        // Register the signal set before starting worker threads so a SIGINT that
        // arrives early is still handled gracefully via io_context.run() rather than
        // taking the default terminate action while workers are live.
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&io_context](const boost::system::error_code&, int) {
            io_context.stop();
        });

        pool.start();

        edgeflow::gateway::Server server(io_context, config.port, queue, config.backpressure);

        std::cout << "edgeflow-gateway listening on port " << config.port
                  << " (workers=" << config.workers
                  << ", queue_capacity=" << config.queue_capacity << ")\n";

        io_context.run();

        pool.stop();
        batcher.flush();
        std::cout << "edgeflow-gateway shut down\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }
}
