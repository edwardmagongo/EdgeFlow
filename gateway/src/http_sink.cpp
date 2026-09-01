#include "edgeflow/gateway/http_sink.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
// For SSL_set_tlsext_host_name, which Asio does not wrap.
#include <openssl/ssl.h>
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
    // Exactly one of these is engaged at a time, chosen by the URL's scheme.
    // Two optionals rather than a variant because the connect paths differ
    // (TLS adds a handshake) while the request path does not -- the exchange
    // below is a generic lambda over whichever stream is live.
    std::optional<beast::tcp_stream> stream;
    std::optional<beast::ssl_stream<beast::tcp_stream>> tls_stream;
    // Built on first TLS use and kept for the thread's lifetime: loading the
    // system trust store is expensive and must not be paid per batch. Held per
    // connection (so per thread) rather than shared, which keeps this free of
    // cross-thread lifetime questions for the cost of one CA load per sink
    // thread at startup.
    std::optional<boost::asio::ssl::context> tls_context;
    // Lives with the connection, not the call: Beast's async_read may pull
    // bytes off the socket beyond the current response into this buffer. On a
    // fresh-per-call buffer those trailing bytes would be silently discarded
    // when the buffer is destroyed, desyncing the next read on the same
    // (reused) socket from the stream. Clearing it in drop() is still
    // required -- a fresh connection must not carry over bytes from a
    // previous, unrelated socket.
    beast::flat_buffer buffer;

    bool live() const { return stream.has_value() || tls_stream.has_value(); }

    // The TCP layer underneath whichever stream is live. expires_after() is a
    // property of that layer, so the timeout handling is identical for both.
    beast::tcp_stream& transport() {
        return tls_stream ? beast::get_lowest_layer(*tls_stream) : *stream;
    }

    // Any transport error drops the connection so the NEXT attempt reconnects.
    // A backend that closed an idle keep-alive socket must cost one batch's
    // retry, never be mistaken for a backend failure -- that distinction is the
    // whole risk of keep-alive and it is handled here.
    void drop() {
        if (tls_stream) {
            // Deliberately no async_shutdown(): a TLS close_notify is a round
            // trip, and this path is reached exactly when the peer is already
            // unresponsive or the deadline has fired. Waiting for a courtesy
            // exchange there would reintroduce the hang expires_after() exists
            // to prevent. Closing the transport is sufficient -- the batch is
            // retried on a fresh connection either way.
            beast::error_code ignored;
            beast::get_lowest_layer(*tls_stream).socket().shutdown(tcp::socket::shutdown_both,
                                                                  ignored);
            beast::get_lowest_layer(*tls_stream).close();
            tls_stream.reset();
        }
        if (stream) {
            beast::error_code ignored;
            stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
            stream->close();
            stream.reset();
        }
        io_context.restart();
        buffer.clear();
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
    try {
        for (std::size_t i = 0; i < options_.concurrency; ++i) {
            threads_.emplace_back([this] { run(); });
        }
    } catch (...) {
        // A constructor that throws never runs ~HttpSink(), so the threads
        // already spawned would be destroyed while still joinable by
        // ~vector<std::thread>, and that calls std::terminate() -- turning a
        // reportable failure (std::thread out of resources at a large
        // --sink-concurrency) into an abort. stop() applies the same drain and
        // join the destructor would have, so the original exception can
        // propagate as an ordinary error instead.
        stop();
        throw;
    }
}

HttpSink::~HttpSink() { stop(); }

HttpSink::Target HttpSink::parse_url(const std::string& url) {
    // Deliberately minimal: http:// or https://, no auth, no query.
    //
    // https:// was rejected outright through Phase 10, when TLS was a stated
    // non-goal. Phase 11 then put the backend behind CloudFront with the ALB
    // admitting only CloudFront's prefix list, which left the sink unable to
    // reach the deployment this project ships. The scheme is now honoured
    // rather than refused, and it selects a real TLS connection -- never a
    // silent downgrade to plaintext.
    static constexpr const char* kPlain = "http://";
    static constexpr const char* kTls = "https://";

    bool use_tls = false;
    std::size_t prefix_length = 0;
    if (url.rfind(kTls, 0) == 0) {
        use_tls = true;
        prefix_length = std::string(kTls).size();
    } else if (url.rfind(kPlain, 0) == 0) {
        prefix_length = std::string(kPlain).size();
    } else {
        throw std::invalid_argument("--sink-url must start with http:// or https:// , got: " +
                                    url);
    }

    const std::string rest = url.substr(prefix_length);
    const std::size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    if (authority.empty()) {
        throw std::invalid_argument("--sink-url has no host: " + url);
    }

    Target target;
    target.use_tls = use_tls;
    target.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        target.host = authority;
        target.port = use_tls ? "443" : "80";
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
        if (target_.use_tls && !connection.tls_context) {
            namespace ssl = boost::asio::ssl;
            connection.tls_context.emplace(ssl::context::tls_client);
            // Verify the chain against the system trust store, and verify that
            // the certificate actually names the host we asked for. Without the
            // second check the first is close to worthless: any certificate
            // signed by any trusted CA would pass.
            boost::system::error_code trust_error;
            connection.tls_context->set_default_verify_paths(trust_error);
            if (trust_error) {
                // No usable CA store means nothing can be verified. Fail the
                // batch rather than fall back to an unverified connection.
                connection.tls_context.reset();
                return SendOutcome::RetryableFailure;
            }
            connection.tls_context->set_verify_mode(ssl::verify_peer);
        }

        if (target_.use_tls) {
            connection.tls_stream.emplace(connection.io_context, *connection.tls_context);
            // SNI. CloudFront and every other host that serves many names off
            // one address needs this to pick a certificate at all; without it
            // the handshake fails or yields the wrong chain.
            if (SSL_set_tlsext_host_name(connection.tls_stream->native_handle(),
                                         target_.host.c_str()) != 1) {
                connection.drop();
                return SendOutcome::RetryableFailure;
            }
            connection.tls_stream->set_verify_callback(
                boost::asio::ssl::host_name_verification(target_.host));
        } else {
            connection.stream.emplace(connection.io_context);
        }
        connection.transport().expires_after(options_.timeout);
        // async_connect, not the synchronous connect() overload: per the
        // comment above (and confirmed against basic_stream.hpp -- the sync
        // overload forwards straight to a plain net::connect() with no timer
        // involved), a synchronous connect on tcp_stream does NOT honour
        // expires_after(). A backend that silently drops the SYN instead of
        // refusing it would otherwise hang this thread past the configured
        // timeout, undermining the bounded-shutdown guarantee HttpSink::stop()
        // depends on.
        boost::system::error_code connect_error;
        connection.transport().async_connect(
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

        if (target_.use_tls) {
            // The handshake is its own async operation and needs its own
            // run(), so the io_context is restarted between the two. It runs
            // under the same expires_after() deadline set before connect --
            // deliberately, since a peer that completes the TCP handshake and
            // then stalls mid-negotiation is exactly the hang this bounds.
            connection.io_context.restart();
            beast::error_code handshake_error;
            connection.tls_stream->async_handshake(
                boost::asio::ssl::stream_base::client,
                [&](beast::error_code error) { handshake_error = error; });
            connection.io_context.run();
            if (handshake_error) {
                // Covers an untrusted chain, a name mismatch, and a plaintext
                // server that never speaks TLS at all. All retryable as far as
                // this sink can tell, and none of them fall back to cleartext.
                connection.drop();
                return SendOutcome::RetryableFailure;
            }
        }
    }

    connection.transport().expires_after(options_.timeout);

    http::request<http::string_body> request{http::verb::post, target_.path, 11};
    request.set(http::field::host, target_.host);
    request.set(http::field::user_agent, "edgeflow-gateway");
    request.set(http::field::content_type, "application/x-ndjson");
    request.set("Idempotency-Key", batch.idempotency_key);
    request.body() = batch.body;
    request.prepare_payload();

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
    // Generic over the stream type: Beast's async_write/async_read take any
    // AsyncStream, and ssl_stream is one. Writing this once rather than twice
    // keeps the plaintext and TLS paths from drifting -- the timeout, the
    // buffer reuse and the error handling below are shared by construction.
    const auto exchange = [&](auto& active_stream) {
        http::async_write(active_stream, request,
                          [&](beast::error_code write_error, std::size_t) {
                              if (write_error) {
                                  failure = write_error;
                                  return;
                              }
                              http::async_read(active_stream, connection.buffer, response,
                                               [&](beast::error_code read_error, std::size_t) {
                                                   if (read_error) {
                                                       failure = read_error;
                                                   }
                                               });
                          });
    };
    if (connection.tls_stream) {
        exchange(*connection.tls_stream);
    } else {
        exchange(*connection.stream);
    }
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
    // exchange, not a plain read-then-write: stop() is public and documented
    // idempotent, and the destructor also calls it. A plain bool made that
    // promise good only for a single calling thread.
    if (stopped_.exchange(true)) {
        return;
    }
    drain_deadline_ = std::chrono::steady_clock::now() + kDrainDeadline;
    draining_.store(true, std::memory_order_release);
    outbound_.shutdown();
    // Join ALL sink threads. shutdown() makes pop() return nullopt once the
    // queue is drained, so every thread finishes the backlog rather than
    // abandoning it.
    //
    // The deadline bounds when a thread STARTS another batch, not when it
    // finishes one: run() checks it before entering send_with_retries(), which
    // never re-checks it. A batch that enters the send path just under the
    // deadline can therefore hold stop() for one more worst-case retry chain
    // (with production defaults, 4 x --sink-timeout-ms plus backoff) on top of
    // kDrainDeadline. Tightening that means re-checking the deadline per retry
    // attempt; it is deliberately left alone here rather than changed without
    // a test that can observe it.
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
