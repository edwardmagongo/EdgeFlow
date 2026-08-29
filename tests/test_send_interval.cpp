// The send interval was computed in whole milliseconds, so any per-device rate
// that did not divide 1000 evenly was silently rounded to a DIFFERENT rate --
// and always upwards, because truncating the interval shortens it. At 400
// events/sec the interval truncated from 2.5ms to 2ms, making the client send
// at 500/sec; at 800/sec it truncated from 1.25ms to 1ms, demanding 1000/sec.
// That capped the whole simulated fleet at ~500,000 events/sec regardless of
// thread count and made the saturation ladder's top rungs meaningless.
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include "edgeflow/simulator/device_client.hpp"

using edgeflow::simulator::DeviceClient;

namespace {
// Constructing a client does not connect; start() does. So these cases need no
// server and no timing.
std::shared_ptr<DeviceClient> make(boost::asio::io_context& io, double rate) {
    return std::make_shared<DeviceClient>(io, "127.0.0.1", 9000, 1, rate);
}
} // namespace

TEST(SendInterval, ExactForRatesThatDivideEvenly) {
    boost::asio::io_context io;
    EXPECT_EQ(make(io, 1000.0)->send_interval(), std::chrono::microseconds(1000));
    EXPECT_EQ(make(io, 100.0)->send_interval(), std::chrono::microseconds(10000));
    EXPECT_EQ(make(io, 1.0)->send_interval(), std::chrono::microseconds(1000000));
}

TEST(SendInterval, ExactForRatesThatMillisecondsCannotRepresent) {
    boost::asio::io_context io;
    // 2.5ms and 1.25ms -- the two cases that silently became 2ms and 1ms.
    EXPECT_EQ(make(io, 400.0)->send_interval(), std::chrono::microseconds(2500));
    EXPECT_EQ(make(io, 800.0)->send_interval(), std::chrono::microseconds(1250));
}

TEST(SendInterval, SupportsRatesAboveOneThousandPerSecond) {
    boost::asio::io_context io;
    // Previously every one of these collapsed onto the same 1ms floor.
    EXPECT_EQ(make(io, 2000.0)->send_interval(), std::chrono::microseconds(500));
    EXPECT_EQ(make(io, 10000.0)->send_interval(), std::chrono::microseconds(100));
}

TEST(SendInterval, ClampsExtremesInsteadOfOverflowing) {
    boost::asio::io_context io;
    // A tiny rate makes 1e6/rate exceed any integer type; a huge rate drives it
    // to zero, which would spin the timer. Both are clamped rather than cast
    // out of range (casting an out-of-range double to an integer is UB).
    EXPECT_EQ(make(io, 1e-300)->send_interval(), std::chrono::microseconds(3'600'000'000LL));
    EXPECT_EQ(make(io, 1e12)->send_interval(), std::chrono::microseconds(1));
}
