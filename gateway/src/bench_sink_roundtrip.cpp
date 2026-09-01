// Phase 10 Task 1: attribution harness for the HTTP sink's round trip.
//
// Answers "where does round_trip_time actually go?" before anything is
// optimised, so Tasks 3 and 4 are gated on measurement rather than intuition.
//
// DELIBERATE DEVIATION FROM PRODUCTION, stated rather than hidden: HttpSink's
// send_once() uses ASYNC Asio operations purely so beast::tcp_stream's
// expires_after() deadline applies -- with synchronous calls the timeout is
// silently ignored. This harness uses SYNCHRONOUS calls, because splitting a
// round trip into timed phases is what it exists to do and no timeout fires
// against a healthy backend. The per-step costs are therefore the same; what
// is missing is only the deadline machinery, which is not on the happy path.
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "edgeflow/event.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

struct Steps {
    double resolve_ms = 0;
    double connect_ms = 0;
    double write_ms = 0;
    double wait_ms = 0;   // time to first response byte -- the backend's own work
    double read_ms = 0;
    double total_ms = 0;
};

struct Target {
    std::string host;
    std::string port;
    std::string path;
};

Target parse_url(const std::string& url) {
    static constexpr const char* kPrefix = "http://";
    if (url.rfind(kPrefix, 0) != 0) {
        throw std::invalid_argument("--url must start with http:// , got: " + url);
    }
    const std::string rest = url.substr(std::string(kPrefix).size());
    const std::size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    Target target;
    target.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        target.host = authority;
        target.port = "80";
    } else {
        target.host = authority.substr(0, colon);
        target.port = authority.substr(colon + 1);
    }
    return target;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Body byte-identical in shape to what HttpSink sends: one serialised event
// per line, trailing newline on every line.
std::string make_body(std::size_t rows) {
    std::string body;
    for (std::size_t i = 0; i < rows; ++i) {
        body += edgeflow::serialize_event(
            edgeflow::Event{static_cast<std::int64_t>(9000 + (i % 100)),
                            "2026-08-31T00:00:00Z", 20.5, 90, 37.7749, -122.4194,
                            "telemetry"});
        body += '\n';
    }
    return body;
}

bool one_round_trip(const Target& target, const std::string& body,
                    std::uint64_t sequence, Steps& steps) {
    const auto total_start = Clock::now();
    boost::asio::io_context io_context;
    tcp::resolver resolver(io_context);
    boost::system::error_code error;

    auto start = Clock::now();
    const auto endpoints = resolver.resolve(target.host, target.port, error);
    steps.resolve_ms = ms_since(start);
    if (error) return false;

    tcp::socket socket(io_context);
    start = Clock::now();
    boost::asio::connect(socket, endpoints, error);
    steps.connect_ms = ms_since(start);
    if (error) return false;

    http::request<http::string_body> request{http::verb::post, target.path, 11};
    request.set(http::field::host, target.host);
    request.set(http::field::user_agent, "edgeflow-bench-sink");
    request.set(http::field::content_type, "application/x-ndjson");
    request.set("Idempotency-Key", "bench-" + std::to_string(sequence));
    request.body() = body;
    request.prepare_payload();

    start = Clock::now();
    http::write(socket, request, error);
    steps.write_ms = ms_since(start);
    if (error) return false;

    // Time to first byte: everything between "request is on the wire" and "the
    // backend started answering" is the backend's own processing.
    start = Clock::now();
    socket.wait(tcp::socket::wait_read, error);
    steps.wait_ms = ms_since(start);
    if (error) return false;

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    start = Clock::now();
    http::read(socket, buffer, response, error);
    steps.read_ms = ms_since(start);
    if (error) return false;

    socket.shutdown(tcp::socket::shutdown_both, error);
    steps.total_ms = ms_since(total_start);
    return response.result_int() >= 200 && response.result_int() < 300;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * (values.size() - 1));
    return values[index];
}

void report(const std::string& label, std::vector<double> values, double total_median) {
    const double med = median(values);
    const double share = total_median > 0 ? (med / total_median) * 100.0 : 0.0;
    std::printf("  %-8s median %7.3f ms  p90 %7.3f ms  min %7.3f ms  max %7.3f ms  %5.1f%%\n",
                label.c_str(), med, percentile(values, 0.90),
                *std::min_element(values.begin(), values.end()),
                *std::max_element(values.begin(), values.end()), share);
}

std::string arg_value(const std::string& arg) {
    return arg.substr(arg.find('=') + 1);
}

} // namespace

int main(int argc, char** argv) {
    std::string url = "http://127.0.0.1:3000/v1/events";
    std::size_t reps = 500;
    std::size_t rows = 100;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("--url=")) url = arg_value(arg);
        else if (arg.starts_with("--reps=")) reps = std::stoul(arg_value(arg));
        else if (arg.starts_with("--rows=")) rows = std::stoul(arg_value(arg));
        else { std::cerr << "unknown argument: " << arg << "\n"; return 2; }
    }

    const Target target = parse_url(url);
    const std::string body = make_body(rows);
    std::printf("url: %s\nrows per batch: %zu, reps: %zu\n", url.c_str(), rows, reps);

    std::vector<double> resolve, connect, write, wait, read, total;
    std::uint64_t sequence = 0;
    std::size_t failures = 0;

    // Warm up: the first round trip pays one-time costs (ARP, route cache,
    // backend lazy init) that would skew a median taken over few reps.
    Steps warmup{};
    one_round_trip(target, body, sequence++, warmup);

    for (std::size_t i = 0; i < reps; ++i) {
        Steps steps{};
        if (!one_round_trip(target, body, sequence++, steps)) { ++failures; continue; }
        resolve.push_back(steps.resolve_ms);
        connect.push_back(steps.connect_ms);
        write.push_back(steps.write_ms);
        wait.push_back(steps.wait_ms);
        read.push_back(steps.read_ms);
        total.push_back(steps.total_ms);
    }

    if (total.empty()) {
        std::cerr << "every round trip failed -- is the backend running?\n";
        return 1;
    }

    const double total_median = median(total);
    std::printf("successful reps: %zu, failed: %zu\n", total.size(), failures);
    report("resolve", resolve, total_median);
    report("connect", connect, total_median);
    report("write", write, total_median);
    report("wait", wait, total_median);
    report("read", read, total_median);
    report("TOTAL", total, total_median);

    const double handshake_share =
        ((median(resolve) + median(connect)) / total_median) * 100.0;
    const double wait_share = (median(wait) / total_median) * 100.0;
    std::printf("\nGATE ANSWERS\n");
    std::printf("  Task 3 (concurrency): wait is %.1f%% of the round trip.\n", wait_share);
    std::printf("  Task 4 (keep-alive):  resolve+connect is %.1f%% of the round trip.\n",
                handshake_share);
    return 0;
}
