#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

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

private:
    void connect();
    void schedule_next_send();
    bool should_drop_for_chaos();
    void send_event();
    void do_send();

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
    std::mt19937 chaos_rng_;
    std::uniform_real_distribution<double> chaos_dist_{0.0, 100.0};
    bool stopped_ = false;
    bool write_in_flight_ = false;
};

} // namespace edgeflow::simulator
