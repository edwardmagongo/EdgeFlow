#include "edgeflow/gateway/connection.hpp"
#include <istream>
#include <utility>

namespace edgeflow::gateway {

using boost::asio::ip::tcp;

Connection::Connection(tcp::socket socket,
                        edgeflow::BoundedQueue<edgeflow::Event>& queue,
                        edgeflow::BackpressurePolicy policy,
                        edgeflow::Stats& stats)
    : socket_(std::move(socket)),
      queue_(queue),
      policy_(policy),
      stats_(stats),
      retry_timer_(socket_.get_executor()) {}

void Connection::start() { read_line(); }

void Connection::read_line() {
    auto self = shared_from_this();
    boost::asio::async_read_until(socket_, buffer_, '\n',
        [this, self](const boost::system::error_code& error, std::size_t) {
            if (error) {
                return; // connection closed or errored; drop it
            }
            std::istream stream(&buffer_);
            std::string line;
            std::getline(stream, line);
            handle_line(line);
        });
}

void Connection::handle_line(const std::string& line) {
    auto event = edgeflow::parse_event(line);
    if (!event) {
        stats_.record_malformed();
        read_line();
        return;
    }
    push_event(std::move(*event));
}

void Connection::push_event(edgeflow::Event event) {
    auto result = queue_.push(event);
    if (result == edgeflow::PushResult::Accepted) {
        stats_.record_accepted();
        read_line();
        return;
    }
    if (result == edgeflow::PushResult::DroppedOldest) {
        stats_.record_dropped_oldest();
        read_line();
        return;
    }

    // RejectedBackpressure.
    if (policy_ == edgeflow::BackpressurePolicy::DropNewest) {
        stats_.record_dropped_newest();
        read_line(); // silently drop; keep reading
        return;
    }

    // Block policy: pause reading, retry the push shortly.
    retry_push(std::move(event));
}

void Connection::retry_push(edgeflow::Event event) {
    auto self = shared_from_this();
    retry_timer_.expires_after(std::chrono::milliseconds(5));
    retry_timer_.async_wait([this, self, event = std::move(event)](const boost::system::error_code& error) mutable {
        if (error) {
            return;
        }
        auto result = queue_.push(event);
        if (result == edgeflow::PushResult::RejectedBackpressure) {
            retry_push(std::move(event));
            return;
        }
        if (result == edgeflow::PushResult::Accepted) {
            stats_.record_accepted();
        } else {
            stats_.record_dropped_oldest();
        }
        read_line();
    });
}

} // namespace edgeflow::gateway
