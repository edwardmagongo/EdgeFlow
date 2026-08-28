// Regression test for the write-in-flight bug fixed in Task 8: DeviceClient
// used to be able to start a second async_write while a previous one was
// still in flight if the send timer fired again before the write completed,
// interleaving/corrupting the bytes on the wire. This test drives a real
// DeviceClient at a high send rate against a real loopback TCP acceptor and
// asserts every complete NDJSON line received is valid, parseable JSON —
// exactly the property the bug would have violated.
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "edgeflow/event.hpp"
#include "edgeflow/simulator/device_client.hpp"

using boost::asio::ip::tcp;

namespace {

// Reads raw bytes from an accepted socket into a buffer and splits complete
// newline-delimited lines out of it as they arrive. Intentionally not a full
// edgeflow::gateway::Connection/Server — just enough to observe the raw byte
// stream a DeviceClient produces.
class RawLineCollector {
public:
    explicit RawLineCollector(boost::asio::io_context& io_context)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), 0)) {}

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start_accept() {
        acceptor_.async_accept(socket_, [this](const boost::system::error_code& error) {
            if (error) {
                return;
            }
            read_more();
        });
    }

    // Lines completed so far (each parsed successfully or not is checked by
    // the caller after io_context has stopped).
    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

private:
    void read_more() {
        socket_.async_read_some(boost::asio::buffer(read_buf_),
            [this](const boost::system::error_code& error, std::size_t bytes) {
                if (error) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending_.append(read_buf_.data(), bytes);
                    std::size_t pos;
                    while ((pos = pending_.find('\n')) != std::string::npos) {
                        lines_.push_back(pending_.substr(0, pos));
                        pending_.erase(0, pos + 1);
                    }
                }
                read_more();
            });
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_{acceptor_.get_executor()};
    std::array<char, 4096> read_buf_{};
    mutable std::mutex mutex_;
    std::string pending_;
    std::vector<std::string> lines_;
};

} // namespace

TEST(DeviceClientIntegration, HighRateSendProducesOnlyValidNdjsonLines) {
    boost::asio::io_context io_context;
    RawLineCollector collector(io_context);
    collector.start_accept();

    auto client = std::make_shared<edgeflow::simulator::DeviceClient>(
        io_context, "127.0.0.1", collector.port(), /*device_id=*/1,
        /*events_per_second=*/1000.0);
    client->start();

    std::thread io_thread([&io_context] { io_context.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // DeviceClient (like Connection) assumes io_context.run() is called from
    // a single thread and does no internal synchronization: calling
    // client->stop() directly from this (the test) thread while io_thread is
    // concurrently running the io_context would race on the timer/socket.
    // Post the stop onto the io_context instead, same as any other
    // cross-thread interaction with asio objects.
    boost::asio::post(io_context, [client] { client->stop(); });

    // Give the collector a moment to drain whatever is still in flight, then
    // stop the io_context and join the thread. io_context::stop() itself is
    // documented thread-safe to call from any thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io_context.stop();
    io_thread.join();

    auto lines = collector.lines();
    ASSERT_FALSE(lines.empty()) << "expected at least one event to be received";

    for (const auto& line : lines) {
        auto parsed = edgeflow::parse_event(line);
        EXPECT_TRUE(parsed.has_value())
            << "line failed to parse as a valid event (possible interleaved/corrupted write): "
            << line;
    }
}
