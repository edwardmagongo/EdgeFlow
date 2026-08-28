#include "edgeflow/simulator/device_client.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include "edgeflow/event.hpp"

namespace edgeflow::simulator {

using boost::asio::ip::tcp;

DeviceClient::DeviceClient(boost::asio::io_context& io_context,
                            const std::string& host,
                            std::uint16_t port,
                            std::int64_t device_id,
                            double events_per_second,
                            std::chrono::milliseconds chaos_latency,
                            double chaos_packet_loss_percent)
    : socket_(io_context),
      resolver_(io_context),
      timer_(io_context),
      chaos_latency_timer_(io_context),
      host_(host),
      port_(port),
      device_id_(device_id),
      // Clamp the interval in double space before casting to long: for a
      // tiny events_per_second (e.g. 1e-300), 1000.0/events_per_second is
      // +inf, and casting an out-of-range double to long is undefined
      // behavior. Clamping first guarantees the cast operand is always
      // in-range (1 to 3,600,000).
      send_interval_(std::chrono::milliseconds(
          static_cast<long>(std::clamp(1000.0 / events_per_second, 1.0, 3'600'000.0)))),
      chaos_latency_(chaos_latency),
      chaos_packet_loss_percent_(chaos_packet_loss_percent),
      chaos_rng_(static_cast<std::mt19937::result_type>(device_id)) {
    build_line_template();
}

void DeviceClient::start() { connect(); }

void DeviceClient::stop() {
    stopped_ = true;
    boost::system::error_code ignored;
    socket_.close(ignored);
    timer_.cancel();
    chaos_latency_timer_.cancel();
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
        if (!write_in_flight_ && !should_drop_for_chaos()) {
            send_event();
        }
        schedule_next_send();
    });
}

bool DeviceClient::should_drop_for_chaos() {
    if (chaos_packet_loss_percent_ <= 0.0) {
        return false;
    }
    return chaos_dist_(chaos_rng_) < chaos_packet_loss_percent_;
}

void DeviceClient::send_event() {
    write_in_flight_ = true;
    if (chaos_latency_.count() <= 0) {
        do_send();
        return;
    }
    auto self = shared_from_this();
    chaos_latency_timer_.expires_after(chaos_latency_);
    chaos_latency_timer_.async_wait([this, self](const boost::system::error_code& error) {
        if (error || stopped_) {
            write_in_flight_ = false;
            return;
        }
        do_send();
    });
}

void DeviceClient::build_line_template() {
    edgeflow::Event event{
        device_id_,
        kPlaceholder,
        20.0 + static_cast<double>(device_id_ % 15),
        static_cast<int>(100 - (device_id_ % 100)),
        37.7749,
        -122.4194,
        "telemetry",
    };
    line_ = edgeflow::serialize_event(event) + "\n";
    timestamp_offset_ = line_.find(kPlaceholder);
    // This is a structural guarantee, not input validation: if it fails, the
    // in-place splice below would corrupt every event this client sends, so
    // fail loudly at construction instead. (The placeholder's length matching
    // kTimestampWidth is instead checked at compile time -- see the
    // static_assert next to kPlaceholder's definition in the header.)
    if (timestamp_offset_ == std::string::npos) {
        throw std::logic_error("timestamp placeholder missing from serialised event");
    }
}

void DeviceClient::do_send() {
    const std::string& timestamp = timestamp_.now();
    if (timestamp.size() == kTimestampWidth) {
        std::memcpy(line_.data() + timestamp_offset_, timestamp.data(), kTimestampWidth);
    }
    ++events_sent_;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(line_),
        [this, self](const boost::system::error_code&, std::size_t) {
            write_in_flight_ = false;
        });
}

} // namespace edgeflow::simulator
