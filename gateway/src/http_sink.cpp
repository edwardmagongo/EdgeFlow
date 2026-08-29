#include "edgeflow/gateway/http_sink.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace edgeflow::gateway {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

HttpSink::HttpSink(Options options, edgeflow::Stats& stats)
    : options_(std::move(options)),
      target_(parse_url(options_.url)),
      stats_(stats),
      outbound_(options_.outbound_capacity, options_.backpressure) {
    thread_ = std::thread([this] { run(); });
}

HttpSink::~HttpSink() { stop(); }

HttpSink::Target HttpSink::parse_url(const std::string& url) {
    // Deliberately minimal: http:// only, no auth, no query. TLS is a non-goal
    // of this phase, so an https:// URL is rejected loudly rather than silently
    // treated as plaintext.
    static constexpr const char* kPrefix = "http://";
    if (url.rfind(kPrefix, 0) != 0) {
        throw std::invalid_argument("--sink-url must start with http:// , got: " + url);
    }
    const std::string rest = url.substr(std::string(kPrefix).size());
    const std::size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    if (authority.empty()) {
        throw std::invalid_argument("--sink-url has no host: " + url);
    }

    Target target;
    target.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        target.host = authority;
        target.port = "80";
    } else {
        target.host = authority.substr(0, colon);
        target.port = authority.substr(colon + 1);
        if (target.host.empty() || target.port.empty()) {
            throw std::invalid_argument("--sink-url has a malformed host:port: " + url);
        }
    }
    return target;
}

void HttpSink::consume(const std::vector<edgeflow::Event>& batch) {
    if (batch.empty()) {
        return;
    }
    // Same bytes FileSink writes: one serialised event per line, trailing
    // newline on every line including the last.
    std::string body;
    for (const auto& event : batch) {
        body += edgeflow::serialize_event(event);
        body += '\n';
    }
    // Non-blocking under the drop-* policies; see the note on the class.
    const auto result = outbound_.push(std::move(body));
    if (result == edgeflow::PushResult::RejectedBackpressure ||
        result == edgeflow::PushResult::DroppedOldest) {
        stats_.record_batch_dropped_outbound();
    }
}

void HttpSink::run() {
    // pop() blocks until a batch arrives or shutdown() is called, so this loop
    // never spins.
    while (auto body = outbound_.pop()) {
        if (send_with_retries(*body)) {
            stats_.record_batch_sent();
        } else {
            stats_.record_batch_dropped_exhausted();
        }
    }
}

HttpSink::SendOutcome HttpSink::send_once(const std::string& body) {
    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        beast::tcp_stream stream(io_context);

        const auto endpoints = resolver.resolve(target_.host, target_.port);
        stream.connect(endpoints);

        http::request<http::string_body> request{http::verb::post, target_.path, 11};
        request.set(http::field::host, target_.host);
        request.set(http::field::user_agent, "edgeflow-gateway");
        request.set(http::field::content_type, "application/x-ndjson");
        request.body() = body;
        request.prepare_payload();
        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);

        boost::system::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

        const unsigned status = response.result_int();
        if (status >= 200 && status < 300) {
            return SendOutcome::Success;
        }
        if (status >= 500 || status == 429) {
            // The backend may recover.
            return SendOutcome::RetryableFailure;
        }
        // Every other 4xx, and every 3xx. A malformed or unauthorised request
        // will not become valid by repetition, and a redirect is deliberately
        // not followed: re-POSTing telemetry to an unconfigured host is worse
        // than a visible failure.
        return SendOutcome::PermanentFailure;
    } catch (const std::exception&) {
        // Refused, reset, resolve failure, or a read that never completed.
        return SendOutcome::RetryableFailure;
    }
}

bool HttpSink::send_with_retries(const std::string& body) {
    std::chrono::milliseconds backoff = options_.backoff_base;
    for (std::size_t attempt = 0; attempt <= options_.max_retries; ++attempt) {
        const SendOutcome outcome = send_once(body);
        if (outcome == SendOutcome::Success) {
            return true;
        }
        if (outcome == SendOutcome::PermanentFailure) {
            return false; // retrying cannot help
        }
        if (attempt == options_.max_retries) {
            return false; // out of attempts
        }
        // ADDITIVE jitter: wait the full backoff, plus up to half of it again.
        // Jitter must never shorten the wait -- backoff is a floor, not an
        // average -- or a backend that is failing under load gets hit sooner
        // than intended. The spread still de-synchronises several gateways
        // retrying against one recovering backend.
        std::uniform_int_distribution<long long> distribution(0, backoff.count() / 2);
        const auto delay = backoff + std::chrono::milliseconds(distribution(jitter_rng_));
        std::this_thread::sleep_for(delay);

        stats_.record_batch_retried();
        backoff = std::min(backoff * 2, std::chrono::milliseconds(30'000));
    }
    return false;
}

void HttpSink::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    // shutdown() makes pop() return nullopt once the queue is drained, so the
    // sink thread finishes the backlog rather than abandoning it.
    outbound_.shutdown();
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace edgeflow::gateway
