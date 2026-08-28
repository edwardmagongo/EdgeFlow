#include "edgeflow/file_sink.hpp"
#include <stdexcept>
#include "edgeflow/event.hpp"

namespace edgeflow {

FileSink::FileSink(const std::string& path)
    : file_(path, std::ios::app) {
    if (!file_.is_open()) {
        throw std::runtime_error("FileSink: could not open file: " + path);
    }
}

void FileSink::consume(const std::vector<Event>& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& event : batch) {
        file_ << serialize_event(event) << '\n';
    }
    file_.flush();
}

} // namespace edgeflow
