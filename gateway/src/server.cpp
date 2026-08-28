#include "server.hpp"
#include <iostream>
#include "connection.hpp"

namespace edgeflow::gateway {

using boost::asio::ip::tcp;

Server::Server(boost::asio::io_context& io_context,
               std::uint16_t port,
               edgeflow::BoundedQueue<edgeflow::Event>& queue,
               edgeflow::BackpressurePolicy policy)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      retry_timer_(io_context),
      queue_(queue),
      policy_(policy) {
    accept();
}

void Server::accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& error, tcp::socket socket) {
            if (error) {
                if (error == boost::asio::error::operation_aborted) {
                    // Acceptor was intentionally closed/destroyed (shutdown); do not re-arm.
                    return;
                }
                // Resource exhaustion (e.g. EMFILE/ENFILE) or other transient error:
                // log it and back off before retrying instead of spinning the CPU.
                std::cerr << "edgeflow-gateway: accept failed: " << error.message() << '\n';
                retry_timer_.expires_after(std::chrono::milliseconds(100));
                retry_timer_.async_wait([this](const boost::system::error_code& timer_error) {
                    if (timer_error) {
                        return;
                    }
                    accept();
                });
                return;
            }
            std::make_shared<Connection>(std::move(socket), queue_, policy_)->start();
            accept();
        });
}

} // namespace edgeflow::gateway
