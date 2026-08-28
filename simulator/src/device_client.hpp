#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace edgeflow::simulator {

// One simulated device: connects once, then sends a synthetic telemetry
// event on a repeating timer until stop() is called.
class DeviceClient : public std::enable_shared_from_this<DeviceClient> {
public:
    DeviceClient(boost::asio::io_context& io_context,
                 const std::string& host,
                 std::uint16_t port,
                 std::int64_t device_id,
                 double events_per_second);

    void start();
    void stop();

private:
    void connect();
    void schedule_next_send();
    void send_event();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer timer_;
    std::string host_;
    std::uint16_t port_;
    std::int64_t device_id_;
    std::chrono::milliseconds send_interval_;
    bool stopped_ = false;
};

} // namespace edgeflow::simulator
