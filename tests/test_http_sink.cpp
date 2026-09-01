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

TEST(StubHttpServer, HandlesOverlappingConnectionsConcurrently) {
    StubHttpServer server;
    // Every reply waits 200ms. Served concurrently, four requests take ~200ms
    // total; served one at a time they take ~800ms.
    server.script({StubResponse{StubAction::Ok, std::chrono::milliseconds(200)}});

    constexpr std::size_t kClients = 4;
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> clients;
    for (std::size_t i = 0; i < kClients; ++i) {
        clients.emplace_back([&server] {
            boost::asio::io_context io_context;
            tcp::socket socket(io_context);
            boost::system::error_code error;
            socket.connect(
                tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), server.port()),
                error);
            ASSERT_FALSE(error);
            http::request<http::string_body> request{http::verb::post, "/batches", 11};
            request.set(http::field::host, "127.0.0.1");
            request.body() = "x\n";
            request.prepare_payload();
            http::write(socket, request, error);
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(socket, buffer, response, error);
        });
    }
    for (auto& client : clients) client.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_EQ(server.request_count(), kClients);
    // Serialized would be ~800ms. Half of that is a wide margin that still
    // cannot be reached without real concurrency.
    EXPECT_LT(elapsed, std::chrono::milliseconds(200 * kClients / 2));
}

#include "edgeflow/file_sink.hpp"
#include "edgeflow/gateway/http_sink.hpp"
#include "edgeflow/stats.hpp"
#include <algorithm>
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

TEST(HttpSink, GivesUpOnARequestThatExceedsTheTimeout) {
    // The stub sits on the request for 2s; the sink is told to wait 200ms.
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(2000)}});
    edgeflow::Stats stats;

    const auto started = std::chrono::steady_clock::now();
    {
        auto options = options_for(server.port());
        options.timeout = std::chrono::milliseconds(200);
        options.max_retries = 0;
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        sink.stop();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::milliseconds(1500))
        << "the sink waited out the backend instead of timing out";
    EXPECT_EQ(stats.snapshot().batches_sent, 0u);
    EXPECT_EQ(stats.snapshot().batches_dropped_exhausted, 1u);
}

TEST(HttpSink, ATimeoutIsRetryable) {
    // The stub serves each CONNECTION on its own thread (Task 2), but a retry
    // reuses the same persistent connection (Task 3) and therefore the same
    // handler thread -- so the first response's delay still blocks the retry
    // behind it. The numbers below are chosen around that: the first response
    // is slow enough to blow a 150ms deadline, but the backoff is long enough
    // that the handler is free again by the time the retry arrives. Making the
    // first delay much larger would simply starve every retry and test
    // nothing.
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(300)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.timeout = std::chrono::milliseconds(150);
        options.max_retries = 2;
        options.backoff_base = std::chrono::milliseconds(400);
        HttpSink sink(options, stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }

    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
    EXPECT_GE(stats.snapshot().batches_retried, 1u);
}

TEST(HttpSink, ShutdownDoesNotHangOnAnUnreachableBackend) {
    edgeflow::Stats stats;
    HttpSink::Options options;
    options.url = "http://127.0.0.1:9/batches"; // discard port -- refused
    options.outbound_capacity = 256;
    options.max_retries = 3;
    options.backoff_base = std::chrono::milliseconds(200);

    const auto started = std::chrono::steady_clock::now();
    {
        HttpSink sink(options, stats);
        for (int i = 0; i < 100; ++i) {
            sink.consume(sample_batch());
        }
        sink.stop();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(12))
        << "stop() took "
        << std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
        << "s; the shutdown drain is not bounded";
}

TEST(HttpSink, ShutdownDrainsWhatItCanAndCountsTheRest) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(50)}});
    edgeflow::Stats stats;

    {
        auto options = options_for(server.port());
        options.outbound_capacity = 256;
        HttpSink sink(options, stats);
        for (int i = 0; i < 10; ++i) {
            sink.consume(sample_batch());
        }
        sink.stop();
    }

    auto snapshot = stats.snapshot();
    const auto accounted = snapshot.batches_sent + snapshot.batches_dropped_outbound +
                           snapshot.batches_dropped_exhausted;
    EXPECT_EQ(accounted, 10u)
        << "every batch must end up in exactly one counter: sent, dropped at the "
           "outbound queue, or dropped as exhausted";
}

TEST(HttpSink, SendsAnIdempotencyKey) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 1));
    }

    auto keys = server.idempotency_keys();
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_FALSE(keys[0].empty()) << "no Idempotency-Key header reached the backend";
}

TEST(HttpSink, ReusesTheSameKeyAcrossARetry) {
    // THE test for this task. A key minted per attempt instead of per batch
    // would still pass every other test in this file -- the batch is delivered,
    // the counters are right -- while making backend deduplication impossible.
    // Only comparing the two keys catches it.
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

    auto keys = server.idempotency_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], keys[1])
        << "the retry minted a fresh key, so the backend cannot tell it is a retry";
    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
}

TEST(HttpSink, GivesDifferentBatchesDifferentKeys) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink sink(options_for(server.port()), stats);
        sink.consume(sample_batch());
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }

    auto keys = server.idempotency_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_NE(keys[0], keys[1])
        << "two distinct batches shared a key; the backend would discard the second";
}

TEST(HttpSink, DrainsTheOutboundQueueConcurrently) {
    StubHttpServer server;
    // Every reply waits 200ms, so the wall time is dominated by how many
    // requests are in flight at once rather than by any local work.
    server.script({StubResponse{StubAction::Ok, std::chrono::milliseconds(200)}});
    edgeflow::Stats stats;

    constexpr std::size_t kBatches = 4;
    const auto started = std::chrono::steady_clock::now();
    {
        auto options = options_for(server.port());
        options.concurrency = kBatches;
        HttpSink sink(std::move(options), stats);
        for (std::size_t i = 0; i < kBatches; ++i) {
            sink.consume(sample_batch());
        }
    } // destructor stops and drains
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_EQ(server.request_count(), kBatches);
    EXPECT_EQ(stats.snapshot().batches_sent, kBatches);
    // Serialized: ~800ms. Concurrent: ~200ms. The midpoint is a wide margin
    // that a single-threaded sink cannot reach.
    EXPECT_LT(elapsed, std::chrono::milliseconds(200 * kBatches / 2));
}

TEST(HttpSink, ConcurrentBatchesEachCarryTheirOwnKey) {
    StubHttpServer server;
    edgeflow::Stats stats;
    constexpr std::size_t kBatches = 16;
    {
        auto options = options_for(server.port());
        options.concurrency = 4;
        HttpSink sink(std::move(options), stats);
        for (std::size_t i = 0; i < kBatches; ++i) {
            sink.consume(sample_batch());
        }
    }
    auto keys = server.idempotency_keys();
    ASSERT_EQ(keys.size(), kBatches);
    // Compare as a SET, not a sequence: under concurrency the arrival order of
    // distinct batches is deliberately not guaranteed, so asserting order here
    // would be asserting something the design explicitly does not promise.
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(std::unique(keys.begin(), keys.end()), keys.end())
        << "two concurrently-sent batches shared an idempotency key";
    EXPECT_EQ(stats.snapshot().batches_sent, kBatches);
}

TEST(HttpSink, StopAccountsForEveryBatchAcrossAllThreads) {
    StubHttpServer server;
    edgeflow::Stats stats;
    constexpr std::size_t kBatches = 32;
    {
        auto options = options_for(server.port());
        options.concurrency = 4;
        HttpSink sink(std::move(options), stats);
        for (std::size_t i = 0; i < kBatches; ++i) {
            sink.consume(sample_batch());
        }
    } // destructor drains within kDrainDeadline
    const auto snapshot = stats.snapshot();
    // Every batch is accounted for exactly once: delivered, dropped on the way
    // into the queue, or abandoned at the drain deadline. Nothing vanishes.
    EXPECT_EQ(snapshot.batches_sent + snapshot.batches_dropped_outbound +
                  snapshot.batches_dropped_exhausted,
              kBatches);
}

TEST(HttpSink, ReusesOneConnectionPerThread) {
    StubHttpServer server;
    edgeflow::Stats stats;
    constexpr std::size_t kBatches = 8;
    {
        auto options = options_for(server.port());
        options.concurrency = 1;  // one thread => one connection for all 8 batches
        HttpSink sink(std::move(options), stats);
        for (std::size_t i = 0; i < kBatches; ++i) sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, kBatches));
    }
    EXPECT_EQ(server.request_count(), kBatches);
    EXPECT_EQ(server.connection_count(), 1u)
        << "each batch opened its own connection -- keep-alive is not in effect";
}

TEST(HttpSink, ConcurrencyOneMatchesTheSingleThreadedContract) {
    StubHttpServer server;
    edgeflow::Stats stats;
    {
        auto options = options_for(server.port());
        options.concurrency = 1;
        HttpSink sink(std::move(options), stats);
        sink.consume(sample_batch());
        sink.consume(sample_batch());
        ASSERT_TRUE(wait_for_requests(server, 2));
    }
    // One thread means one request in flight, so arrival order IS creation
    // order here -- the property concurrency deliberately gives up.
    EXPECT_EQ(server.request_count(), 2u);
    EXPECT_EQ(stats.snapshot().batches_sent, 2u);
}

// --- TLS (Finding 10) -------------------------------------------------------
//
// Phase 5 rejected https:// outright because TLS was a non-goal. Phase 11 then
// deployed the backend behind CloudFront with the ALB admitting only
// CloudFront's prefix list, so the only way in is HTTPS. Each decision was
// right alone; together they left the gateway unable to reach the very backend
// this project deploys.

TEST(HttpSink, AcceptsAnHttpsUrl) {
    edgeflow::Stats stats;
    HttpSink::Options options;
    options.url = "https://example.invalid/v1/events";
    options.concurrency = 1;

    // Construction parses the URL. It must no longer reject the scheme the
    // deployment actually requires.
    EXPECT_NO_THROW({ HttpSink sink(std::move(options), stats); });
}

TEST(HttpSink, StillRejectsAnUnsupportedScheme) {
    edgeflow::Stats stats;
    HttpSink::Options options;
    options.url = "ftp://example.invalid/v1/events";
    options.concurrency = 1;

    EXPECT_THROW({ HttpSink sink(std::move(options), stats); }, std::invalid_argument);
}

// The test that proves TLS is real rather than a scheme the parser now waves
// through. The stub speaks plaintext HTTP; an https:// sink pointed at it must
// fail the handshake and give up, NOT deliver the batch. If this ever reports a
// batch sent, the gateway is putting telemetry on the wire in the clear while
// claiming otherwise -- which is worse than the rejection it replaced.
TEST(HttpSink, DoesNotFallBackToCleartextWhenTheUrlIsHttps) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});
    edgeflow::Stats stats;

    {
        HttpSink::Options options;
        options.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/batches";
        options.concurrency = 1;
        options.max_retries = 0;
        options.backoff_base = std::chrono::milliseconds(1);
        options.timeout = std::chrono::milliseconds(1500);
        HttpSink sink(std::move(options), stats);
        sink.consume(sample_batch());
    }

    EXPECT_EQ(stats.snapshot().batches_sent, 0u);
    EXPECT_EQ(stats.snapshot().batches_dropped_exhausted, 1u);
}

// Disabled by default: it needs the public internet, and ctest must stay
// hermetic. This is the test that actually closes Finding 10 -- everything
// above proves the sink refuses to downgrade, not that it can complete a real
// handshake. Run it deliberately:
//
//   ./tests/edgeflow-tests --gtest_also_run_disabled_tests \
//       --gtest_filter='HttpSink.DISABLED_ReachesARealHttpsEndpoint'
//
// A 2xx here means the whole chain worked: SNI, certificate verification
// against the system trust store, the handshake, and an HTTP exchange over the
// encrypted stream. That is exactly what a CloudFront endpoint requires, so it
// stands in for the deployed backend without needing the stack to be up.
TEST(HttpSink, DISABLED_ReachesARealHttpsEndpoint) {
    edgeflow::Stats stats;

    {
        HttpSink::Options options;
        options.url = "https://postman-echo.com/post";
        options.concurrency = 1;
        options.max_retries = 1;
        options.timeout = std::chrono::milliseconds(15000);
        HttpSink sink(std::move(options), stats);
        sink.consume(sample_batch());
    }

    EXPECT_EQ(stats.snapshot().batches_sent, 1u);
    EXPECT_EQ(stats.snapshot().batches_dropped_exhausted, 0u);
}
