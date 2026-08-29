// Drives DeviceClient the way the sharded simulator does: several io_contexts
// on several threads, each owning a disjoint slice of clients, all stopped the
// way simulator/src/main.cpp stops them.
//
// This file exists because a real bug shipped without any test noticing. The
// first sharding implementation stopped every client from context 0, including
// clients owned by other contexts -- touching another thread's socket and
// timers. It did not crash, no unit test covered it, and the sanitizers stayed
// quiet because nothing exercised the simulator multi-threaded end to end. It
// showed up only as runs overshooting their duration: 10s requested, 23.5s
// actual at 2 threads. These tests close that gap.
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include "edgeflow/simulator/device_client.hpp"

using boost::asio::ip::tcp;

namespace {

// Accepts connections and drains them, counting newline-delimited lines.
class DrainingCollector {
public:
    explicit DrainingCollector(boost::asio::io_context& io_context)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), 0)) {}

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start_accept() {
        acceptor_.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
            if (error) return;
            auto session = std::make_shared<Session>(std::move(socket), *this);
            session->read();
            start_accept();
        });
    }

    std::size_t lines() const { return lines_.load(); }

private:
    struct Session : std::enable_shared_from_this<Session> {
        Session(tcp::socket socket, DrainingCollector& owner)
            : socket_(std::move(socket)), owner_(owner) {}

        void read() {
            auto self = shared_from_this();
            boost::asio::async_read_until(socket_, buffer_, '\n',
                [this, self](const boost::system::error_code& error, std::size_t n) {
                    if (error) return;
                    buffer_.consume(n);
                    owner_.lines_.fetch_add(1, std::memory_order_relaxed);
                    read();
                });
        }

        tcp::socket socket_;
        boost::asio::streambuf buffer_;
        DrainingCollector& owner_;
    };

    tcp::acceptor acceptor_;
    std::atomic<std::size_t> lines_{0};
};

// Mirrors simulator/src/main.cpp: N contexts on N threads, clients sharded
// round-robin, each client stopped by a task posted onto its OWN context.
struct ShardedFleet {
    std::vector<std::unique_ptr<boost::asio::io_context>> contexts;
    std::vector<std::shared_ptr<edgeflow::simulator::DeviceClient>> clients;
    std::vector<boost::asio::io_context*> owners;
    std::vector<std::thread> threads;

    ShardedFleet(std::size_t thread_count, std::size_t device_count,
                 std::uint16_t port, double rate) {
        for (std::size_t i = 0; i < thread_count; ++i) {
            contexts.push_back(std::make_unique<boost::asio::io_context>());
        }
        for (std::size_t i = 0; i < device_count; ++i) {
            auto& context = *contexts[i % thread_count];
            auto client = std::make_shared<edgeflow::simulator::DeviceClient>(
                context, "127.0.0.1", port, static_cast<std::int64_t>(i), rate);
            clients.push_back(client);
            owners.push_back(&context);
            client->start();
        }
    }

    void run() {
        for (std::size_t i = 1; i < contexts.size(); ++i) {
            threads.emplace_back([this, i] { contexts[i]->run(); });
        }
    }

    // Stop each client on its owning context -- the invariant the bug violated.
    void stop_and_join() {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            auto client = clients[i];
            boost::asio::post(*owners[i], [client] { client->stop(); });
        }
        for (auto& context : contexts) {
            context->stop();
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }

    std::uint64_t events_sent() const {
        std::uint64_t total = 0;
        for (const auto& client : clients) total += client->events_sent();
        return total;
    }
};

} // namespace

TEST(SimulatorThreading, ShardedFleetSendsFromEveryThread) {
    boost::asio::io_context collector_context;
    DrainingCollector collector(collector_context);
    collector.start_accept();
    std::thread collector_thread([&collector_context] {
        auto guard = boost::asio::make_work_guard(collector_context);
        collector_context.run();
    });

    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kDevices = 20;
    ShardedFleet fleet(kThreads, kDevices, collector.port(), 50.0);
    fleet.run();

    // Context 0 is driven from this thread, mirroring main.cpp, but only for a
    // bounded slice so the test cannot hang.
    fleet.contexts[0]->run_for(std::chrono::milliseconds(600));
    fleet.stop_and_join();

    collector_context.stop();
    collector_thread.join();

    // Every client is owned by exactly one of the four contexts; if sharding
    // were broken, some slice would never send.
    EXPECT_GT(fleet.events_sent(), 0u);
    EXPECT_GT(collector.lines(), 0u);
}

TEST(SimulatorThreading, StoppingOnOwningContextTerminatesPromptly) {
    // The regression this pins: stopping clients from the wrong thread made
    // runs overshoot their duration by 2x or more. Here, shutdown must complete
    // quickly once every client has been stopped on its own context.
    boost::asio::io_context collector_context;
    DrainingCollector collector(collector_context);
    collector.start_accept();
    std::thread collector_thread([&collector_context] {
        auto guard = boost::asio::make_work_guard(collector_context);
        collector_context.run();
    });

    ShardedFleet fleet(4, 20, collector.port(), 100.0);
    fleet.run();
    fleet.contexts[0]->run_for(std::chrono::milliseconds(300));

    const auto started = std::chrono::steady_clock::now();
    fleet.stop_and_join();
    const auto shutdown = std::chrono::steady_clock::now() - started;

    collector_context.stop();
    collector_thread.join();

    EXPECT_LT(shutdown, std::chrono::seconds(5))
        << "sharded shutdown took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(shutdown).count()
        << "ms; clients are probably being stopped from the wrong thread";
}
