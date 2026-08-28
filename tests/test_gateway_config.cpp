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
