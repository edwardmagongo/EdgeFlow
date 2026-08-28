#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <istream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include "edgeflow/event.hpp"
#include "edgeflow/queue_concept.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow::gateway {

// One TCP connection: reads newline-delimited JSON events and pushes them
// onto the shared queue. On BackpressurePolicy::Block, when the queue
// rejects a push, the connection stops reading and retries the push on a
// short timer instead of blocking the I/O thread.
//
// Assumes io_context.run() is called from a single thread; this class is not
// internally synchronized for a multi-threaded io_context (no strand). That
// single-threaded reactor is also why production has exactly one producer.
//
// Templated on the queue for the same reason WorkerPool is: compile-time
// selection, no virtual dispatch on the push path.
template <edgeflow::EventQueue Q>
class Connection : public std::enable_shared_from_this<Connection<Q>> {
    static_assert(std::is_same_v<typename Q::value_type, edgeflow::TimedEvent>,
                  "Connection pushes TimedEvent so queue-wait latency can be measured");

public:
    Connection(boost::asio::ip::tcp::socket socket,
               Q& queue,
               edgeflow::BackpressurePolicy policy,
               edgeflow::Stats& stats)
        : socket_(std::move(socket)),
          queue_(queue),
          policy_(policy),
          stats_(stats),
          retry_timer_(socket_.get_executor()) {}

    void start() { read_line(); }

private:
    void read_line() {
        // enable_shared_from_this is a dependent base now, hence `this->`.
        auto self = this->shared_from_this();
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

    void handle_line(const std::string& line) {
        auto event = edgeflow::parse_event(line);
        if (!event) {
            stats_.record_malformed();
            read_line();
            return;
        }
        push_event(edgeflow::TimedEvent{std::move(*event), std::chrono::steady_clock::now()});
    }

    void push_event(edgeflow::TimedEvent timed) {
        auto result = queue_.push(timed);
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

        // Block policy: pause reading, retry the push shortly. The original
        // enqueued_at timestamp travels with `timed` across retries, so queue-wait
        // latency measures total time including any backpressure delay.
        retry_push(std::move(timed));
    }

    void retry_push(edgeflow::TimedEvent timed) {
        auto self = this->shared_from_this();
        retry_timer_.expires_after(std::chrono::milliseconds(5));
        retry_timer_.async_wait([this, self, timed = std::move(timed)](const boost::system::error_code& error) mutable {
            if (error) {
                return;
            }
            auto result = queue_.push(timed);
            if (result == edgeflow::PushResult::RejectedBackpressure) {
                retry_push(std::move(timed));
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

    boost::asio::ip::tcp::socket socket_;
    // Capped at 1 MB so a client that never sends a newline can't grow this
    // buffer unboundedly and exhaust memory.
    boost::asio::streambuf buffer_{1 << 20};
    Q& queue_;
    edgeflow::BackpressurePolicy policy_;
    edgeflow::Stats& stats_;
    boost::asio::steady_timer retry_timer_;
};

} // namespace edgeflow::gateway
