#include "edgeflow/simulator/device_client.hpp"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "edgeflow/event.hpp"

namespace edgeflow::simulator {

using boost::asio::ip::tcp;

namespace {
std::string iso_timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    gmtime_r(&time, &utc_tm);
    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
} // namespace

DeviceClient::DeviceClient(boost::asio::io_context& io_context,
                            const std::string& host,
                            std::uint16_t port,
                            std::int64_t device_id,
                            double events_per_second)
    : socket_(io_context),
      resolver_(io_context),
      timer_(io_context),
      host_(host),
      port_(port),
      device_id_(device_id),
      // Clamp the interval in double space before casting to long: for a
      // tiny events_per_second (e.g. 1e-300), 1000.0/events_per_second is
      // +inf, and casting an out-of-range double to long is undefined
      // behavior. Clamping first guarantees the cast operand is always
      // in-range (1 to 3,600,000).
      send_interval_(std::chrono::milliseconds(
          static_cast<long>(std::clamp(1000.0 / events_per_second, 1.0, 3'600'000.0)))) {}

void DeviceClient::start() { connect(); }

void DeviceClient::stop() {
    stopped_ = true;
    boost::system::error_code ignored;
    socket_.close(ignored);
    timer_.cancel();
}

void DeviceClient::connect() {
    auto self = shared_from_this();
    auto endpoints = resolver_.resolve(host_, std::to_string(port_));
    boost::asio::async_connect(socket_, endpoints,
        [this, self](const boost::system::error_code& error, const tcp::endpoint&) {
            if (error || stopped_) {
                return;
            }
            schedule_next_send();
        });
}

void DeviceClient::schedule_next_send() {
    if (stopped_) return;
    auto self = shared_from_this();
    timer_.expires_after(send_interval_);
    timer_.async_wait([this, self](const boost::system::error_code& error) {
        if (error || stopped_) return;
        if (!write_in_flight_) {
            send_event();
        }
        schedule_next_send();
    });
}

void DeviceClient::send_event() {
    edgeflow::Event event{
        device_id_,
        iso_timestamp_now(),
        20.0 + static_cast<double>(device_id_ % 15),
        static_cast<int>(100 - (device_id_ % 100)),
        37.7749,
        -122.4194,
        "telemetry",
    };
    auto line = std::make_shared<std::string>(edgeflow::serialize_event(event) + "\n");
    auto self = shared_from_this();
    write_in_flight_ = true;
    boost::asio::async_write(socket_, boost::asio::buffer(*line),
        [this, self, line](const boost::system::error_code&, std::size_t) {
            write_in_flight_ = false;
        });
}

} // namespace edgeflow::simulator
