#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include "edgeflow/iso_timestamp.hpp"

namespace edgeflow::simulator {

// One simulated device: connects once, then sends a synthetic telemetry
// event on a repeating timer until stop() is called. Optionally injects
// chaos: extra per-event latency before sending, and/or randomly skips
// sends to simulate a device that intermittently fails to produce telemetry
// (not true network packet loss -- TCP delivers reliably once bytes reach
// the socket).
class DeviceClient : public std::enable_shared_from_this<DeviceClient> {
public:
    DeviceClient(boost::asio::io_context& io_context,
                 const std::string& host,
                 std::uint16_t port,
                 std::int64_t device_id,
                 double events_per_second,
                 std::chrono::milliseconds chaos_latency = std::chrono::milliseconds(0),
                 double chaos_packet_loss_percent = 0.0);

    void start();
    void stop();

    // Total events written to the socket by this client. Incremented only on the
    // owning io_context thread; read after that thread has finished.
    std::uint64_t events_sent() const { return events_sent_; }

private:
    void connect();
    void schedule_next_send();
    bool should_drop_for_chaos();
    void send_event();
    void do_send();
    void build_line_template();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer timer_;
    boost::asio::steady_timer chaos_latency_timer_;
    std::string host_;
    std::uint16_t port_;
    std::int64_t device_id_;
    std::chrono::milliseconds send_interval_;
    std::chrono::milliseconds chaos_latency_;
    double chaos_packet_loss_percent_;
    // One per client. DeviceClient is not shared across io_context threads, so
    // this needs no synchronisation.
    edgeflow::CachedIsoTimestamp timestamp_;
    std::uint64_t events_sent_ = 0;
    std::mt19937 chaos_rng_;
    std::uniform_real_distribution<double> chaos_dist_{0.0, 100.0};
    bool stopped_ = false;
    bool write_in_flight_ = false;

    // The device's serialised NDJSON line, built once. Only the 20-byte
    // timestamp at timestamp_offset_ changes between sends. Written directly by
    // async_write, which is safe because write_in_flight_ allows only one
    // outstanding write per client.
    std::string line_;
    std::size_t timestamp_offset_ = 0;
    // "2026-08-28T12:34:56Z" -- fixed width, so the splice is a memcpy.
    static constexpr std::size_t kTimestampWidth = 20;
    // A fixed-width placeholder of exactly the same shape a real timestamp
    // has, so it can be overwritten in place. It cannot collide with any
    // other value in the line.
    static constexpr const char* kPlaceholder = "0000-00-00T00:00:00Z";
    // The placeholder's length vs. kTimestampWidth is fully determined at
    // compile time, so check it here instead of at runtime on every device
    // construction -- a build where this doesn't hold can't compile.
    static_assert(std::string_view{kPlaceholder}.size() == kTimestampWidth,
                  "placeholder length must match the fixed timestamp width");
};

} // namespace edgeflow::simulator
