#include <boost/asio.hpp>
#include <csignal>
#include <iostream>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/config.hpp"
#include "edgeflow/worker_pool.hpp"
#include "server.hpp"

namespace {
boost::asio::io_context* g_io_context = nullptr;

void handle_signal(int) {
    if (g_io_context) {
        g_io_context->stop();
    }
}
} // namespace

int main(int argc, char** argv) {
    edgeflow::gateway::Config config;
    try {
        config = edgeflow::gateway::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-gateway: " << e.what() << '\n';
        return 1;
    }

    edgeflow::BoundedQueue<edgeflow::Event> queue(config.queue_capacity, config.backpressure);
    edgeflow::FileSink sink(config.sink_file);
    edgeflow::Batcher batcher(config.batch_size, config.batch_age,
                                [&sink](std::vector<edgeflow::Event> batch) { sink.consume(batch); });
    edgeflow::WorkerPool pool(queue, batcher, config.workers);
    pool.start();

    boost::asio::io_context io_context;
    g_io_context = &io_context;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    edgeflow::gateway::Server server(io_context, config.port, queue, config.backpressure);

    std::cout << "edgeflow-gateway listening on port " << config.port
              << " (workers=" << config.workers
              << ", queue_capacity=" << config.queue_capacity << ")\n";

    io_context.run();

    pool.stop();
    batcher.flush();
    std::cout << "edgeflow-gateway shut down\n";
    return 0;
}
