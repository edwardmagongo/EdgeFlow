#pragma once
#include <boost/asio.hpp>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"

namespace edgeflow::gateway {

class Server {
public:
    Server(boost::asio::io_context& io_context,
           std::uint16_t port,
           edgeflow::BoundedQueue<edgeflow::Event>& queue,
           edgeflow::BackpressurePolicy policy);

private:
    void accept();

    boost::asio::ip::tcp::acceptor acceptor_;
    edgeflow::BoundedQueue<edgeflow::Event>& queue_;
    edgeflow::BackpressurePolicy policy_;
};

} // namespace edgeflow::gateway
