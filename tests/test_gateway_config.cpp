#include <gtest/gtest.h>
#include <stdexcept>
#include "edgeflow/gateway/config.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::gateway::parse_args;

TEST(GatewayConfig, DefaultsAreSane) {
    char prog[] = "edgeflow-gateway";
    char* argv[] = {prog};
    auto config = parse_args(1, argv);
    EXPECT_EQ(config.port, 9000);
    EXPECT_EQ(config.workers, 4u);
    EXPECT_EQ(config.backpressure, BackpressurePolicy::Block);
}

TEST(GatewayConfig, ParsesOverrides) {
    char prog[] = "edgeflow-gateway";
    char port[] = "--port=8080";
    char workers[] = "--workers=8";
    char policy[] = "--backpressure=drop-oldest";
    char* argv[] = {prog, port, workers, policy};
    auto config = parse_args(4, argv);
    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.workers, 8u);
    EXPECT_EQ(config.backpressure, BackpressurePolicy::DropOldest);
}

TEST(GatewayConfig, RejectsZeroWorkers) {
    char prog[] = "edgeflow-gateway";
    char workers[] = "--workers=0";
    char* argv[] = {prog, workers};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsUnknownFlag) {
    char prog[] = "edgeflow-gateway";
    char bogus[] = "--nope=1";
    char* argv[] = {prog, bogus};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsNegativeWorkers) {
    char prog[] = "edgeflow-gateway";
    char workers[] = "--workers=-1";
    char* argv[] = {prog, workers};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsPortAboveRange) {
    char prog[] = "edgeflow-gateway";
    char port[] = "--port=70000";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsPortZero) {
    char prog[] = "edgeflow-gateway";
    char port[] = "--port=0";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsPortWithTrailingJunk) {
    char prog[] = "edgeflow-gateway";
    char port[] = "--port=8080junk";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsZeroQueueCapacity) {
    char prog[] = "edgeflow-gateway";
    char capacity[] = "--queue-capacity=0";
    char* argv[] = {prog, capacity};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsUnknownBackpressureValue) {
    char prog[] = "edgeflow-gateway";
    char policy[] = "--backpressure=nonsense";
    char* argv[] = {prog, policy};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, DefaultsToTheFileSink) {
    char prog[] = "edgeflow-gateway";
    char* argv[] = {prog};
    auto config = parse_args(1, argv);
    EXPECT_EQ(config.sink_kind, edgeflow::gateway::SinkKind::File);
    EXPECT_EQ(config.sink_outbound_capacity, 256u);
    EXPECT_EQ(config.sink_max_retries, 3u);
}

TEST(GatewayConfig, ParsesHttpSinkFlags) {
    char prog[] = "edgeflow-gateway";
    char kind[] = "--sink=http";
    char url[] = "--sink-url=http://127.0.0.1:8080/batches";
    char capacity[] = "--sink-outbound-capacity=64";
    char retries[] = "--sink-max-retries=5";
    char backoff[] = "--sink-backoff-ms=250";
    char timeout[] = "--sink-timeout-ms=1500";
    char* argv[] = {prog, kind, url, capacity, retries, backoff, timeout};
    auto config = parse_args(7, argv);

    EXPECT_EQ(config.sink_kind, edgeflow::gateway::SinkKind::Http);
    EXPECT_EQ(config.sink_url, "http://127.0.0.1:8080/batches");
    EXPECT_EQ(config.sink_outbound_capacity, 64u);
    EXPECT_EQ(config.sink_max_retries, 5u);
    EXPECT_EQ(config.sink_backoff_ms.count(), 250);
    EXPECT_EQ(config.sink_timeout_ms.count(), 1500);
}

TEST(GatewayConfig, HttpSinkRequiresAUrl) {
    // Failing at startup beats discovering it when the first batch is dropped.
    char prog[] = "edgeflow-gateway";
    char kind[] = "--sink=http";
    char* argv[] = {prog, kind};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, RejectsAnUnknownSinkKind) {
    char prog[] = "edgeflow-gateway";
    char kind[] = "--sink=carrier-pigeon";
    char* argv[] = {prog, kind};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(GatewayConfig, ParsesSinkBackpressureSeparatelyFromTheEventQueue) {
    // Two queues, configured independently: --backpressure is the event queue,
    // --sink-backpressure is the outbound queue.
    char prog[] = "edgeflow-gateway";
    char event[] = "--backpressure=drop-newest";
    char sink[] = "--sink-backpressure=block";
    char* argv[] = {prog, event, sink};
    auto config = parse_args(3, argv);

    EXPECT_EQ(config.backpressure, edgeflow::BackpressurePolicy::DropNewest);
    EXPECT_EQ(config.sink_backpressure, edgeflow::BackpressurePolicy::Block);
}

TEST(GatewayConfig, RejectsAZeroOutboundCapacity) {
    char prog[] = "edgeflow-gateway";
    char capacity[] = "--sink-outbound-capacity=0";
    char* argv[] = {prog, capacity};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}
