#include "server.hpp"
#include "connection.hpp"

namespace edgeflow::gateway {

using boost::asio::ip::tcp;

Server::Server(boost::asio::io_context& io_context,
               std::uint16_t port,
               edgeflow::BoundedQueue<edgeflow::Event>& queue,
               edgeflow::BackpressurePolicy policy)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      queue_(queue),
      policy_(policy) {
    accept();
}

void Server::accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& error, tcp::socket socket) {
            if (!error) {
                std::make_shared<Connection>(std::move(socket), queue_, policy_)->start();
            }
            accept();
        });
}

} // namespace edgeflow::gateway
