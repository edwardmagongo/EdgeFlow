#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow::gateway {

// One TCP connection: reads newline-delimited JSON events and pushes them
// onto the shared queue. On BackpressurePolicy::Block, when the queue
// rejects a push, the connection stops reading and retries the push on a
// short timer instead of blocking the I/O thread.
//
// Assumes io_context.run() is called from a single thread; this class is not
// internally synchronized for a multi-threaded io_context (no strand).
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(boost::asio::ip::tcp::socket socket,
               edgeflow::BoundedQueue<edgeflow::TimedEvent>& queue,
               edgeflow::BackpressurePolicy policy,
               edgeflow::Stats& stats);

    void start();

private:
    void read_line();
    void handle_line(const std::string& line);
    void push_event(edgeflow::TimedEvent timed);
    void retry_push(edgeflow::TimedEvent timed);

    boost::asio::ip::tcp::socket socket_;
    // Capped at 1 MB so a client that never sends a newline can't grow this
    // buffer unboundedly and exhaust memory.
    boost::asio::streambuf buffer_{1 << 20};
    edgeflow::BoundedQueue<edgeflow::TimedEvent>& queue_;
    edgeflow::BackpressurePolicy policy_;
    edgeflow::Stats& stats_;
    boost::asio::steady_timer retry_timer_;
};

} // namespace edgeflow::gateway
