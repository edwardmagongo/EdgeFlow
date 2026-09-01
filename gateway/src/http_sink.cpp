#include "edgeflow/gateway/http_sink.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace edgeflow::gateway {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

namespace {

// One live connection per sink thread, reused across batches so the TCP
// handshake is paid once per thread rather than once per batch.
//
// thread_local rather than an HttpSink member: each sink thread must own
// exactly one connection and never share it. Sharing would need a lock around
// the socket, which would re-serialise precisely the requests Task 3
// parallelised -- the change would undo itself.
struct PersistentConnection {
    boost::asio::io_context io_context;
    std::optional<beast::tcp_stream> stream;

    bool live() const { return stream.has_value(); }

    // Any transport error drops the connection so the NEXT attempt reconnects.
    // A backend that closed an idle keep-alive socket must cost one batch's
    // retry, never be mistaken for a backend failure -- that distinction is the
    // whole risk of keep-alive and it is handled here.
    void drop() {
        if (stream) {
            beast::error_code ignored;
            stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
            stream->close();
            stream.reset();
        }
        io_context.restart();
    }
};

} // namespace

HttpSink::HttpSink(Options options, edgeflow::Stats& stats)
    : options_(std::move(options)),
      target_(parse_url(options_.url)),
      stats_(stats),
      outbound_(options_.outbound_capacity, options_.backpressure) {
    // Every thread runs the identical run() loop and pops from the same queue.
    // BoundedQueue is a mutex-guarded deque, so multiple consumers are already
    // safe; Stats is entirely std::atomic. Neither needed a change.
    threads_.reserve(options_.concurrency);
    for (std::size_t i = 0; i < options_.concurrency; ++i) {
        threads_.emplace_back([this] { run(); });
    }
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

std::string HttpSink::make_key_prefix() {
    std::random_device device;
    std::mt19937_64 engine(device());
    std::uniform_int_distribution<std::uint64_t> distribution;
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(distribution(engine)));
    return std::string(buffer);
}

std::string HttpSink::next_idempotency_key() {
    // relaxed is sufficient: the counter only needs to be unique, never ordered
    // against anything else.
    return key_prefix_ + "-" +
           std::to_string(key_counter_.fetch_add(1, std::memory_order_relaxed));
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
    // The key is minted HERE, once, as the batch is created -- not at send
    // time. Every retry of this batch then carries this same value.
    OutboundBatch queued{std::move(body), next_idempotency_key()};
    // Non-blocking under the drop-* policies; see the note on the class.
    const auto result = outbound_.push(std::move(queued));
    if (result == edgeflow::PushResult::RejectedBackpressure ||
        result == edgeflow::PushResult::DroppedOldest) {
        stats_.record_batch_dropped_outbound();
    }
}

void HttpSink::run() {
    // pop() blocks until a batch arrives or shutdown() is called, so this loop
    // never spins.
    while (auto batch = outbound_.pop()) {
        // Once stop() has started, honour the drain deadline: anything still
        // queued past it is counted as dropped rather than silently discarded
        // or retried forever.
        if (draining_.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() >= drain_deadline_) {
            stats_.record_batch_dropped_exhausted();
            continue;
        }
        if (send_with_retries(*batch)) {
            stats_.record_batch_sent();
        } else {
            stats_.record_batch_dropped_exhausted();
        }
    }
}

HttpSink::SendOutcome HttpSink::send_once(const OutboundBatch& batch) {
    // The operations below are ASYNCHRONOUS on purpose. beast::tcp_stream's
    // expires_after() only governs async operations -- with synchronous
    // connect/write/read the timeout is silently ignored and a hung backend
    // parks this thread forever. Everything still runs to completion inside
    // io_context.run() before returning, so the sink thread is not doing
    // anything concurrent; the callbacks are just how a deadline gets applied.
    static thread_local PersistentConnection connection;

    if (!connection.live()) {
        tcp::resolver resolver(connection.io_context);
        boost::system::error_code resolve_error;
        const auto endpoints =
            resolver.resolve(target_.host, target_.port, resolve_error);
        if (resolve_error) {
            return SendOutcome::RetryableFailure;
        }
        connection.stream.emplace(connection.io_context);
        connection.stream->expires_after(options_.timeout);
        // async_connect, not the synchronous connect() overload: per the
        // comment above (and confirmed against basic_stream.hpp -- the sync
        // overload forwards straight to a plain net::connect() with no timer
        // involved), a synchronous connect on tcp_stream does NOT honour
        // expires_after(). A backend that silently drops the SYN instead of
        // refusing it would otherwise hang this thread past the configured
        // timeout, undermining the bounded-shutdown guarantee HttpSink::stop()
        // depends on.
        boost::system::error_code connect_error;
        connection.stream->async_connect(
            endpoints, [&](beast::error_code error, const tcp::endpoint&) {
                connect_error = error;
            });
        connection.io_context.run();
        // Restarted unconditionally below, before the write/read block, so no
        // restart() is needed here between this run() and that one.
        if (connect_error) {
            connection.drop();
            return SendOutcome::RetryableFailure;
        }
    }

    beast::tcp_stream& stream = *connection.stream;
    stream.expires_after(options_.timeout);

    http::request<http::string_body> request{http::verb::post, target_.path, 11};
    request.set(http::field::host, target_.host);
    request.set(http::field::user_agent, "edgeflow-gateway");
    request.set(http::field::content_type, "application/x-ndjson");
    request.set("Idempotency-Key", batch.idempotency_key);
    request.body() = batch.body;
    request.prepare_payload();

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::error_code failure;

    // The io_context is thread_local and reused across batches, so it must be
    // restarted before each fresh set of async operations: a prior run() that
    // ran out of work leaves it in the stopped state, and a second run()
    // without restart() would return immediately without doing anything.
    connection.io_context.restart();

    // Covers write and read together: the deadline is not reset between
    // steps, so a backend that stalls at either point is caught. Connect is
    // no longer part of this call on a reused connection -- it already
    // happened, possibly batches ago.
    http::async_write(stream, request, [&](beast::error_code write_error, std::size_t) {
        if (write_error) {
            failure = write_error;
            return;
        }
        http::async_read(stream, buffer, response,
                         [&](beast::error_code read_error, std::size_t) {
                             if (read_error) {
                                 failure = read_error;
                             }
                         });
    });
    connection.io_context.run();

    if (failure) {
        // Refused, reset, a truncated read, or the deadline firing. All
        // transient as far as this sink can tell, so all retryable. The
        // connection cannot be trusted after a transport-level error, so it
        // is dropped -- the next attempt reconnects rather than reusing a
        // socket left in an unknown state.
        connection.drop();
        return SendOutcome::RetryableFailure;
    }

    // HTTP/1.1 keeps the connection alive by default, so no header change is
    // needed and the wire contract is unchanged. But the backend may still
    // close: honour that rather than reusing a socket it has finished with.
    if (!response.keep_alive()) {
        connection.drop();
    }

    const unsigned status = response.result_int();
    if (status >= 200 && status < 300) {
        return SendOutcome::Success;
    }
    if (status >= 500 || status == 429) {
        // The backend may recover. This is a healthy connection with a bad
        // application-level outcome, so it is NOT dropped -- dropping it here
        // would throw away a good socket on every retryable server error.
        return SendOutcome::RetryableFailure;
    }
    // Every other 4xx, and every 3xx. A malformed or unauthorised request will
    // not become valid by repetition, and a redirect is deliberately not
    // followed: re-POSTing telemetry to an unconfigured host is worse than a
    // visible failure. Also not dropped, for the same reason as the 5xx/429
    // case above.
    return SendOutcome::PermanentFailure;
}

bool HttpSink::send_with_retries(const OutboundBatch& batch) {
    std::chrono::milliseconds backoff = options_.backoff_base;
    for (std::size_t attempt = 0; attempt <= options_.max_retries; ++attempt) {
        const SendOutcome outcome = send_once(batch);
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
        // Jitter source. This was a member documented as sink-thread-only,
        // which stopped being true the moment the sink drained from N threads:
        // concurrent mutation of one mt19937 is a data race, and TSan reports
        // it. thread_local gives each sink thread its own generator with no
        // synchronisation and no contention, and jitter needs no coordination
        // between threads to do its job.
        static thread_local std::mt19937 jitter_rng(std::random_device{}());
        std::uniform_int_distribution<long long> distribution(0, backoff.count() / 2);
        const auto delay = backoff + std::chrono::milliseconds(distribution(jitter_rng));
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
    drain_deadline_ = std::chrono::steady_clock::now() + kDrainDeadline;
    draining_.store(true, std::memory_order_release);
    // shutdown() makes pop() return nullopt once the queue is drained, so the
    // sink thread finishes the backlog rather than abandoning it -- bounded by
    // the deadline checked in run().
    outbound_.shutdown();
    // Join ALL sink threads. shutdown() makes pop() return nullopt once the
    // queue is drained, so every thread finishes the backlog rather than
    // abandoning it -- bounded by the deadline checked in run().
    //
    // drain_deadline_ is written BEFORE draining_ is stored with release, and
    // run() reads it only after an acquire load of draining_. That ordering is
    // what makes one deadline safe to share across N threads; do not reorder
    // these two lines.
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

} // namespace edgeflow::gateway
