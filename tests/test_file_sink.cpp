#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "edgeflow/file_sink.hpp"

using edgeflow::Event;
using edgeflow::FileSink;

namespace {
std::string temp_path() {
    return (std::filesystem::temp_directory_path() / "edgeflow_file_sink_test.ndjson").string();
}
} // namespace

class FileSinkTest : public ::testing::Test {
protected:
    void SetUp() override { std::filesystem::remove(temp_path()); }
    void TearDown() override { std::filesystem::remove(temp_path()); }
};

TEST_F(FileSinkTest, WritesBatchAsNdjson) {
    FileSink sink(temp_path());

    std::vector<Event> batch{
        Event{1, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"},
        Event{2, "2026-08-28T00:00:01Z", 21.0, 99, 0.0, 0.0, "telemetry"},
    };
    sink.consume(batch);

    std::ifstream file(temp_path());
    std::string line;
    int line_count = 0;
    while (std::getline(file, line)) {
        ++line_count;
    }
    EXPECT_EQ(line_count, 2);
}

TEST_F(FileSinkTest, AppendsAcrossMultipleConsumeCalls) {
    FileSink sink(temp_path());
    sink.consume({Event{1, "t", 20.0, 100, 0.0, 0.0, "telemetry"}});
    sink.consume({Event{2, "t", 20.0, 100, 0.0, 0.0, "telemetry"}});

    std::ifstream file(temp_path());
    std::string line;
    int line_count = 0;
    while (std::getline(file, line)) {
        ++line_count;
    }
    EXPECT_EQ(line_count, 2);
}

TEST_F(FileSinkTest, AppendsAcrossSeparateConstructions) {
    {
        FileSink first_sink(temp_path());
        first_sink.consume({Event{1, "t", 20.0, 100, 0.0, 0.0, "telemetry"}});
    } // first_sink destroyed here, file closed

    FileSink second_sink(temp_path());
    second_sink.consume({Event{2, "t", 20.0, 100, 0.0, 0.0, "telemetry"}});

    std::ifstream file(temp_path());
    std::string line;
    int line_count = 0;
    while (std::getline(file, line)) {
        ++line_count;
    }
    EXPECT_EQ(line_count, 2);
}
