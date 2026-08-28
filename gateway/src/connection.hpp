#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"

namespace edgeflow::gateway {

// One TCP connection: reads newline-delimited JSON events and pushes them
// onto the shared queue. On BackpressurePolicy::Block, when the queue
// rejects a push, the connection stops reading and retries the push on a
// short timer instead of blocking the I/O thread.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(boost::asio::ip::tcp::socket socket,
               edgeflow::BoundedQueue<edgeflow::Event>& queue,
               edgeflow::BackpressurePolicy policy);

    void start();

private:
    void read_line();
    void handle_line(const std::string& line);
    void push_event(edgeflow::Event event);
    void retry_push(edgeflow::Event event);

    boost::asio::ip::tcp::socket socket_;
    boost::asio::streambuf buffer_;
    edgeflow::BoundedQueue<edgeflow::Event>& queue_;
    edgeflow::BackpressurePolicy policy_;
    boost::asio::steady_timer retry_timer_;
};

} // namespace edgeflow::gateway
