// Regression test for the write-in-flight bug fixed in Task 8: DeviceClient
// used to be able to start a second async_write while a previous one was
// still in flight if the send timer fired again before the write completed,
// interleaving/corrupting the bytes on the wire. This test drives a real
// DeviceClient at a high send rate against a real loopback TCP acceptor and
// asserts every complete NDJSON line received is valid, parseable JSON --
// exactly the property the bug would have violated.
//
// For this test to actually exercise the write_in_flight_ guard, writes on
// the client side need to genuinely take multiple event-loop turns to
// complete -- otherwise write_in_flight_ is essentially always false by the
// time the next send timer fires and the guard is never contended. A
// continuously-draining loopback reader completes each async_write almost
// instantly, so the collector below deliberately throttles its reads (small
// fixed-size chunks with a delay between them) and shrinks its receive
// buffer, forcing TCP flow control to push back on the writer and making
// async_write on the DeviceClient side back up behind the kernel socket
// buffers. See the validation notes at the bottom of this file for how these
// specific parameters were chosen and what was (and wasn't) observed when
// validating this test against the bug it targets.
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

// Reads raw bytes from an accepted socket in small, deliberately-delayed
// chunks and splits complete newline-delimited lines out of the reassembled
// stream as they arrive. Intentionally not a full
// edgeflow::gateway::Connection/Server -- just enough to observe the raw byte
// stream a DeviceClient produces, under a slow/throttled drain.
class RawLineCollector {
public:
    explicit RawLineCollector(boost::asio::io_context& io_context)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), 0)),
          delay_timer_(io_context) {
        // Shrink the receive buffer on the LISTENING socket, before any
        // connection is accepted, so it is inherited by the accepted socket
        // from the start of the TCP handshake. Setting it after accept() is
        // too late to matter: the receive window already advertised during
        // the handshake (the OS default, often 128KB+) does not shrink
        // retroactively (shrinking an already-advertised TCP window is
        // disallowed, to avoid silly window syndrome), so the writer could
        // still fill far more than the "new" small buffer before any
        // backpressure appeared. With it set here, the throttled reads below
        // make TCP flow control actually push back on the writer once a
        // couple of chunks' worth of data are outstanding, instead of a
        // large default buffer quietly swallowing everything this test could
        // ever produce.
        boost::system::error_code ignored;
        acceptor_.set_option(boost::asio::socket_base::receive_buffer_size(16), ignored);
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start_accept() {
        acceptor_.async_accept(socket_, [this](const boost::system::error_code& error) {
            if (error) {
                return;
            }
            read_chunk();
        });
    }

    // Lines completed so far (each parsed successfully or not is checked by
    // the caller after io_context has stopped).
    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

private:
    void read_chunk() {
        // Exact-size read of a small fixed chunk (not async_read_until) --
        // we want to control precisely how much is drained per cycle so the
        // throttle below is the bottleneck, not however much happened to be
        // available.
        boost::asio::async_read(socket_, boost::asio::buffer(chunk_),
            [this](const boost::system::error_code& error, std::size_t bytes) {
                if (error) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending_.append(chunk_.data(), bytes);
                    std::size_t pos;
                    while ((pos = pending_.find('\n')) != std::string::npos) {
                        lines_.push_back(pending_.substr(0, pos));
                        pending_.erase(0, pos + 1);
                    }
                }
                // Deliberate delay between reads: a continuously-draining
                // reader completes the writer's async_write calls almost
                // instantly on loopback, never exercising the
                // write_in_flight_ guard under contention. Falling behind
                // like this makes the kernel socket buffers (and TCP flow
                // control, given the shrunk SO_RCVBUF above) back up for
                // real -- confirmed during validation (see notes below) to
                // genuinely cause the send timer to fire while a write is
                // still outstanding, thousands of times per run.
                delay_timer_.expires_after(std::chrono::milliseconds(20));
                delay_timer_.async_wait([this](const boost::system::error_code& timer_error) {
                    if (timer_error) {
                        return;
                    }
                    read_chunk();
                });
            });
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_{acceptor_.get_executor()};
    boost::asio::steady_timer delay_timer_;
    std::array<char, 8> chunk_{};
    mutable std::mutex mutex_;
    std::string pending_;
    std::vector<std::string> lines_;
};

} // namespace

TEST(DeviceClientIntegration, HighRateSendUnderSlowReaderProducesOnlyValidNdjsonLines) {
    boost::asio::io_context io_context;
    RawLineCollector collector(io_context);
    collector.start_accept();

    auto client = std::make_shared<edgeflow::simulator::DeviceClient>(
        io_context, "127.0.0.1", collector.port(), /*device_id=*/1,
        /*events_per_second=*/1000.0);
    client->start();

    std::thread io_thread([&io_context] { io_context.run(); });

    // Let the client hammer the deliberately slow-draining collector for
    // long enough that the local kernel send buffer genuinely fills up and
    // async_write on the client side spans multiple event-loop turns -- this
    // is the condition under which the write_in_flight_ guard in
    // schedule_next_send() actually matters. On an unthrottled local reader,
    // writes complete instantly and the guard is never contended. 5 seconds
    // was empirically the point at which contention reliably starts on the
    // local development machine (see validation notes below) -- shorter
    // send phases (down to ~3s) consistently produced zero contention in
    // testing.
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // DeviceClient (like Connection) assumes io_context.run() is called from
    // a single thread and does no internal synchronization: calling
    // client->stop() directly from this (the test) thread while io_thread is
    // concurrently running the io_context would race on the timer/socket.
    // Post the stop onto the io_context instead, same as any other
    // cross-thread interaction with asio objects.
    boost::asio::post(io_context, [client] { client->stop(); });

    // Keep the throttled drain running after the client stops so whatever
    // backed up in the kernel socket buffers during the send phase gets read
    // out and counted, then stop the io_context and join the thread.
    // io_context::stop() itself is documented thread-safe to call from any
    // thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    io_context.stop();
    io_thread.join();

    auto lines = collector.lines();
    // Empirically, this configuration (RCVBUF=16, 8-byte chunks every 20ms,
    // 5s send + 3s drain) consistently yields ~18-21 complete lines on the
    // development machine. Assert a conservative fraction of that as a
    // floor, so a regression that dropped throughput to near-zero -- not
    // just one that corrupted lines -- would also fail this test, while
    // leaving margin for slower/more loaded CI machines.
    ASSERT_GE(lines.size(), 10u)
        << "expected on the order of a couple dozen events under the "
           "throttled reader; only " << lines.size()
        << " arrived (possible throughput regression)";

    for (const auto& line : lines) {
        auto parsed = edgeflow::parse_event(line);
        EXPECT_TRUE(parsed.has_value())
            << "line failed to parse as a valid event (possible interleaved/corrupted write): "
            << line;
    }
}

// --- Validation notes (Fix B, final whole-branch-review fix round) ---------
//
// Per review requirement, the write_in_flight_ guard in
// simulator/src/device_client.cpp's schedule_next_send() was temporarily
// removed and this test was run repeatedly against the unguarded build to
// confirm this test can catch the regression it targets. Summary of what was
// found (see the fix-round report for full detail):
//
//   1. With the guard removed and the throttle parameters above, this test
//      still PASSED every time (no corrupted/unparseable line was ever
//      observed), despite confirming via temporary diagnostic
//      instrumentation that the send timer genuinely fired while a write was
//      still in flight ~1200+ times per run -- i.e. real contention was
//      happening, just not manifesting as observable byte-level corruption.
//   2. A ThreadSanitizer build of this exact test (guard removed) reported no
//      race, which is expected and not meaningful here: this bug is a
//      same-thread reentrancy/protocol violation (two composed async_write
//      operations outstanding on one socket, both driven by a single
//      io_context thread), not a cross-thread data race, so TSan is not the
//      right detector for it.
//   3. A separate, DeviceClient-independent minimal reproduction (two
//      overlapping async_write calls issuing large ~128KB distinguishable
//      payloads on one socket, no timer/guard involved) DID reliably produce
//      observable byte-level interleaving on this same platform -- proving
//      Boost.Asio's reactive backend here is genuinely capable of corrupting
//      overlapping writes, i.e. the hazard the guard defends against is real
//      on this platform.
//   4. The likely reason (1) and (3) differ: DeviceClient's actual per-event
//      NDJSON payload is small (~130-150 bytes), which is small enough to
//      complete within a single underlying write_some() syscall in the vast
//      majority of cases even under significant throttling (small SO_RCVBUF,
//      slow chunked reads), so the specific interleaving window that
//      corrupts large/multi-write_some payloads rarely opens for payloads
//      this small on this OS/reactor combination.
//
// In short: this test could not be made to observably fail from the removed
// guard via black-box byte-stream inspection on this platform, despite
// genuine confirmed contention. What it does verify, and is worth keeping:
// the guard is now exercised under real backpressure (not a no-op as with
// the original unthrottled version of this test), and the strengthened
// line-count assertion catches a throughput collapse. Possible next steps if
// stronger corruption-detection coverage is wanted: run the same experiment
// on Linux/epoll (a different reactor implementation might behave
// differently); inflate the simulated event payload size to force writes to
// span multiple write_some calls more reliably; or add a
// DeviceClient-independent large-payload smoke test (like the minimal
// reproduction used for validation) as a standing regression test for the
// overlapping-async_write hazard itself, decoupled from DeviceClient's
// specific payload size.
