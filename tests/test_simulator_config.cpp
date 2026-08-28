#include <gtest/gtest.h>
#include <stdexcept>
#include "edgeflow/simulator/config.hpp"

using edgeflow::simulator::parse_args;

TEST(SimulatorConfig, DefaultsAreSane) {
    char prog[] = "edgeflow-simulator";
    char* argv[] = {prog};
    auto config = parse_args(1, argv);
    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.port, 9000);
    EXPECT_EQ(config.device_count, 1000u);
}

TEST(SimulatorConfig, ParsesOverrides) {
    char prog[] = "edgeflow-simulator";
    char host[] = "--host=10.0.0.5";
    char devices[] = "--devices=250";
    char rate[] = "--rate=2.5";
    char* argv[] = {prog, host, devices, rate};
    auto config = parse_args(4, argv);
    EXPECT_EQ(config.host, "10.0.0.5");
    EXPECT_EQ(config.device_count, 250u);
    EXPECT_DOUBLE_EQ(config.events_per_second_per_device, 2.5);
}

TEST(SimulatorConfig, RejectsZeroDevices) {
    char prog[] = "edgeflow-simulator";
    char devices[] = "--devices=0";
    char* argv[] = {prog, devices};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsUnknownFlag) {
    char prog[] = "edgeflow-simulator";
    char bogus[] = "--nope=1";
    char* argv[] = {prog, bogus};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsNegativeDevices) {
    char prog[] = "edgeflow-simulator";
    char devices[] = "--devices=-1";
    char* argv[] = {prog, devices};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsDevicesWithTrailingJunk) {
    char prog[] = "edgeflow-simulator";
    char devices[] = "--devices=250junk";
    char* argv[] = {prog, devices};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsNegativeRate) {
    char prog[] = "edgeflow-simulator";
    char rate[] = "--rate=-2.5";
    char* argv[] = {prog, rate};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsZeroRate) {
    char prog[] = "edgeflow-simulator";
    char rate[] = "--rate=0";
    char* argv[] = {prog, rate};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsRateWithTrailingJunk) {
    char prog[] = "edgeflow-simulator";
    char rate[] = "--rate=2.5junk";
    char* argv[] = {prog, rate};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsNegativeDuration) {
    char prog[] = "edgeflow-simulator";
    char duration[] = "--duration=-30";
    char* argv[] = {prog, duration};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsDurationWithTrailingJunk) {
    char prog[] = "edgeflow-simulator";
    char duration[] = "--duration=30junk";
    char* argv[] = {prog, duration};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, ParsesDuration) {
    char prog[] = "edgeflow-simulator";
    char duration[] = "--duration=60";
    char* argv[] = {prog, duration};
    auto config = parse_args(2, argv);
    EXPECT_EQ(config.duration_seconds, 60u);
}

TEST(SimulatorConfig, RejectsPortAboveRange) {
    char prog[] = "edgeflow-simulator";
    char port[] = "--port=70000";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsPortZero) {
    char prog[] = "edgeflow-simulator";
    char port[] = "--port=0";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsPortWithTrailingJunk) {
    char prog[] = "edgeflow-simulator";
    char port[] = "--port=9000junk";
    char* argv[] = {prog, port};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, ParsesPort) {
    char prog[] = "edgeflow-simulator";
    char port[] = "--port=8080";
    char* argv[] = {prog, port};
    auto config = parse_args(2, argv);
    EXPECT_EQ(config.port, 8080);
}

TEST(SimulatorConfig, RejectsInfiniteRate) {
    char prog[] = "edgeflow-simulator";
    char rate[] = "--rate=inf";
    char* argv[] = {prog, rate};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsNanRate) {
    char prog[] = "edgeflow-simulator";
    char rate[] = "--rate=nan";
    char* argv[] = {prog, rate};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsDevicesAboveMaximum) {
    char prog[] = "edgeflow-simulator";
    char devices[] = "--devices=200000";
    char* argv[] = {prog, devices};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, DefaultsIncludeChaosOff) {
    char prog[] = "edgeflow-simulator";
    char* argv[] = {prog};
    auto config = parse_args(1, argv);
    EXPECT_EQ(config.chaos_latency.count(), 0);
    EXPECT_DOUBLE_EQ(config.chaos_packet_loss_percent, 0.0);
    EXPECT_EQ(config.chaos_device_spike_count, 0u);
    EXPECT_EQ(config.chaos_device_spike_at_sec, 0u);
}

TEST(SimulatorConfig, ParsesChaosLatency) {
    char prog[] = "edgeflow-simulator";
    char latency[] = "--chaos-latency-ms=250";
    char* argv[] = {prog, latency};
    auto config = parse_args(2, argv);
    EXPECT_EQ(config.chaos_latency.count(), 250);
}

TEST(SimulatorConfig, RejectsNegativeChaosLatency) {
    char prog[] = "edgeflow-simulator";
    char latency[] = "--chaos-latency-ms=-1";
    char* argv[] = {prog, latency};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, ParsesChaosPacketLossPercent) {
    char prog[] = "edgeflow-simulator";
    char loss[] = "--chaos-packet-loss-percent=25.5";
    char* argv[] = {prog, loss};
    auto config = parse_args(2, argv);
    EXPECT_DOUBLE_EQ(config.chaos_packet_loss_percent, 25.5);
}

TEST(SimulatorConfig, RejectsChaosPacketLossAboveHundred) {
    char prog[] = "edgeflow-simulator";
    char loss[] = "--chaos-packet-loss-percent=150";
    char* argv[] = {prog, loss};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, ParsesChaosDeviceSpike) {
    char prog[] = "edgeflow-simulator";
    char duration[] = "--duration=30";
    char spike[] = "--chaos-device-spike=50";
    char spike_at[] = "--chaos-device-spike-at-sec=10";
    char* argv[] = {prog, duration, spike, spike_at};
    auto config = parse_args(4, argv);
    EXPECT_EQ(config.chaos_device_spike_count, 50u);
    EXPECT_EQ(config.chaos_device_spike_at_sec, 10u);
}

TEST(SimulatorConfig, RejectsDeviceSpikeAtOrAfterDuration) {
    char prog[] = "edgeflow-simulator";
    char duration[] = "--duration=10";
    char spike[] = "--chaos-device-spike=50";
    char spike_at[] = "--chaos-device-spike-at-sec=10";
    char* argv[] = {prog, duration, spike, spike_at};
    EXPECT_THROW(parse_args(4, argv), std::invalid_argument);
}

TEST(SimulatorConfig, DefaultsToOneThread) {
    char prog[] = "edgeflow-simulator";
    char* argv[] = {prog};
    auto config = parse_args(1, argv);
    EXPECT_EQ(config.thread_count, 1u);
}

TEST(SimulatorConfig, ParsesThreadCount) {
    char prog[] = "edgeflow-simulator";
    char threads[] = "--threads=4";
    char* argv[] = {prog, threads};
    auto config = parse_args(2, argv);
    EXPECT_EQ(config.thread_count, 4u);
}

TEST(SimulatorConfig, RejectsZeroThreads) {
    char prog[] = "edgeflow-simulator";
    char threads[] = "--threads=0";
    char* argv[] = {prog, threads};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}

TEST(SimulatorConfig, RejectsAbsurdThreadCount) {
    char prog[] = "edgeflow-simulator";
    char threads[] = "--threads=5000";
    char* argv[] = {prog, threads};
    EXPECT_THROW(parse_args(2, argv), std::invalid_argument);
}
