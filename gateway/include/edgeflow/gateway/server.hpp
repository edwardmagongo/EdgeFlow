#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include "edgeflow/gateway/connection.hpp"
#include "edgeflow/queue_concept.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow::gateway {

// Accepts TCP connections and hands each one to a Connection<Q>.
//
// Templated on the queue so the whole gateway pipeline is monomorphised on one
// queue type, chosen at compile time by which binary is being built.
template <edgeflow::EventQueue Q>
class Server {
    static_assert(std::is_same_v<typename Q::value_type, edgeflow::TimedEvent>,
                  "Server hands its queue to Connection, which pushes TimedEvent");

public:
    Server(boost::asio::io_context& io_context,
           std::uint16_t port,
           Q& queue,
           edgeflow::BackpressurePolicy policy,
           edgeflow::Stats& stats)
        : acceptor_(io_context,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
          retry_timer_(io_context),
          queue_(queue),
          policy_(policy),
          stats_(stats) {
        accept();
    }

private:
    void accept() {
        acceptor_.async_accept(
            [this](const boost::system::error_code& error, boost::asio::ip::tcp::socket socket) {
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
                std::make_shared<Connection<Q>>(std::move(socket), queue_, policy_, stats_)->start();
                accept();
            });
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer retry_timer_;
    Q& queue_;
    edgeflow::BackpressurePolicy policy_;
    edgeflow::Stats& stats_;
};

} // namespace edgeflow::gateway
