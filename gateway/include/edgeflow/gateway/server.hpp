#pragma once
#include <boost/asio.hpp>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow::gateway {

class Server {
public:
    Server(boost::asio::io_context& io_context,
           std::uint16_t port,
           edgeflow::BoundedQueue<edgeflow::TimedEvent>& queue,
           edgeflow::BackpressurePolicy policy,
           edgeflow::Stats& stats);

private:
    void accept();

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer retry_timer_;
    edgeflow::BoundedQueue<edgeflow::TimedEvent>& queue_;
    edgeflow::BackpressurePolicy policy_;
    edgeflow::Stats& stats_;
};

} // namespace edgeflow::gateway
