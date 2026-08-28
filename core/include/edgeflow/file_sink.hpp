#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include "edgeflow/sink.hpp"

namespace edgeflow {

// Appends each batch as newline-delimited JSON to the given file path.
// Safe to call consume() from multiple threads.
class FileSink : public Sink {
public:
    explicit FileSink(const std::string& path);
    void consume(const std::vector<Event>& batch) override;

private:
    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace edgeflow
