#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include "stub_http_server.hpp"

using edgeflow::testing::StubAction;
using edgeflow::testing::StubHttpServer;
using edgeflow::testing::StubResponse;

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// Minimal one-shot client, so the stub's tests do not depend on HttpSink.
// Returns the status code, or 0 if the server hung up without replying.
unsigned post(std::uint16_t port, const std::string& body) {
    boost::asio::io_context io_context;
    tcp::socket socket(io_context);
    socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

    http::request<http::string_body> request{http::verb::post, "/batches", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/x-ndjson");
    request.body() = body;
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code error;
    http::read(socket, buffer, response, error);
    if (error) return 0;
    return response.result_int();
}

} // namespace

TEST(StubHttpServer, RepliesOkAndRecordsTheBody) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "hello\n"), 200u);
    ASSERT_EQ(server.request_count(), 1u);
    EXPECT_EQ(server.bodies().at(0), "hello\n");
    EXPECT_EQ(server.content_types().at(0), "application/x-ndjson");
}

TEST(StubHttpServer, FollowsTheScriptInOrder) {
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)},
                   {StubAction::TooManyRequests, std::chrono::milliseconds(0)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 500u);
    EXPECT_EQ(post(server.port(), "b\n"), 429u);
    EXPECT_EQ(post(server.port(), "c\n"), 200u);
}

TEST(StubHttpServer, RepeatsTheLastEntryPastTheEndOfTheScript) {
    // So "always fail" needs only a one-entry script.
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 500u);
    EXPECT_EQ(post(server.port(), "b\n"), 500u);
    EXPECT_EQ(post(server.port(), "c\n"), 500u);
}

TEST(StubHttpServer, CloseWithoutReplyLooksLikeATransportFailure) {
    StubHttpServer server;
    server.script({{StubAction::CloseWithoutReply, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 0u) << "expected no readable response";
    EXPECT_EQ(server.request_count(), 1u) << "the request should still have been read";
}

#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/http_sink.hpp"
#include "edgeflow/stats.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using edgeflow::Event;
using edgeflow::gateway::HttpSink;

namespace {

std::vector<Event> sample_batch() {
    return {
        Event{1, "2026-08-28T00:00:00Z", 20.5, 90, 37.7749, -122.4194, "telemetry"},
        Event{2, "2026-08-28T00:00:01Z", 21.5, 89, 37.7749, -122.4194, "telemetry"},
    };
}

HttpSink::Options options_for(std::uint16_t port) {
    HttpSink::Options options;
    options.url = "http://127.0.0.1:" + std::to_string(port) + "/batches";
    return options;
}

// Polls until the stub has seen `count` requests, or the deadline passes.
bool wait_for_requests(const StubHttpServer& server, std::size_t count,
                       std::chrono::milliseconds deadline = std::chrono::milliseconds(5000)) {
    const auto until = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < until) {
        if (server.request_count() >= count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST(HttpSink, PostsABatchAndCountsItSent) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 1));
    }

    EXPECT_EQ(server.request_count(), 1u);
    EXPECT_EQ(server.content_types().at(0), "application/x-ndjson");
    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
    EXPECT_EQ(stats.snapshot().batches_dropped_outbound, 0u);
    EXPECT_EQ(stats.snapshot().batches_dropped_exhausted, 0u);
}

TEST(HttpSink, BodyIsByteIdenticalToFileSinkOutput) {
    // The replay-with-curl property: a file written by FileSink can be POSTed
    // to the backend unchanged, because both emit exactly the same bytes.
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;
    const auto batch = sample_batch();

    const std::string path = "/tmp/edgeflow_http_sink_parity.ndjson";
    std::remove(path.c_str());
    {
        edgeflow::FileSink file_sink(path);
        file_sink.consume(batch);
    }
    std::ifstream stream(path);
    std::stringstream file_bytes;
    file_bytes << stream.rdbuf();

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume(batch);
        ASSERT_TRUE(wait_for_requests(server, 1));
    }

    EXPECT_EQ(server.bodies().at(0), file_bytes.str());
}

TEST(HttpSink, SendsEachBatchAsItsOwnRequest) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume(sample_batch());
        sink.consume(sample_batch());
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 3));
    }

    EXPECT_EQ(server.request_count(), 3u);
    EXPECT_EQ(stats.snapshot().batches_sent, 3u);
}

TEST(HttpSink, RetriesAServerErrorThenSucceeds) {
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.batches_sent, 1u);
    EXPECT_EQ(snapshot.batches_retried, 1u) << "one retry attempt";
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 0u);
}

TEST(HttpSink, RetriesA429) {
    StubHttpServer server;
    server.script({{StubAction::TooManyRequests, std::chrono::milliseconds(0)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }

    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
    EXPECT_EQ(stats.snapshot().batches_retried, 1u);
}

TEST(HttpSink, GivesUpAfterMaxRetriesAndCountsExhausted) {
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)}}); // always 500
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.max_retries = 2;
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        // 1 initial attempt + 2 retries = 3 requests.
        ASSERT_TRUE(wait_for_requests(server, 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto snapshot = stats.snapshot();
    EXPECT_EQ(server.request_count(), 3u) << "one initial attempt plus max_retries";
    EXPECT_EQ(snapshot.batches_sent, 0u);
    EXPECT_EQ(snapshot.batches_retried, 2u);
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 1u);
}

TEST(HttpSink, DoesNotRetryA400) {
    // A malformed request will still be malformed on the third attempt.
    StubHttpServer server;
    server.script({{StubAction::BadRequest, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.max_retries = 3;
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto snapshot = stats.snapshot();
    EXPECT_EQ(server.request_count(), 1u) << "4xx must not be retried";
    EXPECT_EQ(snapshot.batches_retried, 0u);
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 1u);
}

TEST(HttpSink, DoesNotFollowRedirects) {
    // Re-POSTing telemetry to an unconfigured host would be worse than failing.
    StubHttpServer server;
    server.script({{StubAction::Redirect, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EXPECT_EQ(server.request_count(), 1u);
    EXPECT_EQ(stats.snapshot().batches_sent, 0u);
    EXPECT_EQ(stats.snapshot().batches_dropped_exhausted, 1u);
}

TEST(HttpSink, RetriesATransportFailure) {
    // The server hangs up without replying: retryable, like a reset connection.
    StubHttpServer server;
    server.script({{StubAction::CloseWithoutReply, std::chrono::milliseconds(0)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.backoff_base = std::chrono::milliseconds(10);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }

    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
    EXPECT_EQ(stats.snapshot().batches_retried, 1u);
}

TEST(HttpSink, BackoffGrowsBetweenAttempts) {
    // Three failing attempts at a 40ms base must wait at least 40 + 80 = 120ms
    // in total. Asserting a lower bound only: scheduling jitter can make it
    // longer, never shorter.
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    const auto started = std::chrono::steady_clock::now();
    {
        auto options = options_for(server.port());
        options.max_retries = 2;
        options.backoff_base = std::chrono::milliseconds(40);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 3));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_GE(elapsed, std::chrono::milliseconds(120))
        << "backoff does not appear to be growing between attempts";
}

TEST(HttpSink, ConsumeDoesNotBlockOnASlowBackend) {
    // THE architectural claim of this phase. A worker thread must hand the batch
    // over and return, whatever the backend is doing.
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(400)}});
    edgeflow::Stats stats;

    auto options = options_for(server.port());
    options.outbound_capacity = 64;
    HttpSink sink(options, stats);

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) {
        sink.consume(sample_batch());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::milliseconds(200))
        << "consume() took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms for 8 batches against a 400ms-per-request backend; it is blocking "
           "on the network instead of queueing";
    sink.stop();
}

TEST(HttpSink, DropsOldestAndCountsWhenTheOutboundQueueFills) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(300)}});
    edgeflow::Stats stats;

    auto options = options_for(server.port());
    options.outbound_capacity = 2;
    options.backpressure = edgeflow::BackpressurePolicy::DropOldest;
    HttpSink sink(options, stats);

    for (int i = 0; i < 40; ++i) {
        sink.consume(sample_batch());
    }

    EXPECT_GT(stats.snapshot().batches_dropped_outbound, 0u)
        << "40 batches into a 2-deep queue behind a 300ms backend dropped nothing";
    sink.stop();
}

TEST(HttpSink, DropNewestAlsoCountsOverflow) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(300)}});
    edgeflow::Stats stats;

    auto options = options_for(server.port());
    options.outbound_capacity = 2;
    options.backpressure = edgeflow::BackpressurePolicy::DropNewest;
    HttpSink sink(options, stats);

    for (int i = 0; i < 40; ++i) {
        sink.consume(sample_batch());
    }

    EXPECT_GT(stats.snapshot().batches_dropped_outbound, 0u);
    sink.stop();
}

TEST(HttpSink, OverflowAndExhaustionAreCountedSeparately) {
    // An outage looks like exhaustion, a burst looks like overflow; conflating
    // them would make the counters useless for diagnosis.
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.outbound_capacity = 64;
        options.max_retries = 0;
        options.backoff_base = std::chrono::milliseconds(1);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 1));
    }

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 1u);
    EXPECT_EQ(snapshot.batches_dropped_outbound, 0u);
    EXPECT_EQ(snapshot.batches_sent, 0u);
}

TEST(HttpSink, EmptyBatchesAreIgnored) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume({});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    EXPECT_EQ(server.request_count(), 0u) << "an empty batch should not become a POST";
    EXPECT_EQ(stats.snapshot().batches_sent, 0u);
}
