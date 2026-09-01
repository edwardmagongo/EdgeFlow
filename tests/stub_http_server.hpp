#pragma once
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edgeflow::testing {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// What the stub should do with one request.
enum class StubAction {
    Ok,                  // 200
    ServerError,         // 500  -- retryable
    TooManyRequests,     // 429  -- retryable
    BadRequest,          // 400  -- NOT retryable
    Redirect,            // 302  -- NOT retryable, must not be followed
    CloseWithoutReply,   // accept, read, then hang up -- transport failure
};

struct StubResponse {
    StubAction action = StubAction::Ok;
    // Delay before replying. Used to drive the timeout tests.
    std::chrono::milliseconds delay{0};
};

// A real HTTP server on an ephemeral loopback port whose reply to each
// successive request is scripted in advance. Once the script is exhausted it
// keeps replying with the last entry, so a test that only cares about "always
// 500" can script a single entry.
//
// Records every request body, Content-Type and Idempotency-Key so tests can
// assert on the exact bytes and headers the sink put on the wire.
class StubHttpServer {
public:
    StubHttpServer()
        : acceptor_(io_context_, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { serve(); });
    }

    ~StubHttpServer() { stop(); }

    StubHttpServer(const StubHttpServer&) = delete;
    StubHttpServer& operator=(const StubHttpServer&) = delete;

    std::uint16_t port() const { return port_; }

    void script(std::vector<StubResponse> responses) {
        std::lock_guard<std::mutex> lock(mutex_);
        script_ = std::move(responses);
    }

    std::vector<std::string> bodies() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bodies_;
    }

    std::vector<std::string> content_types() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return content_types_;
    }

    std::vector<std::string> idempotency_keys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idempotency_keys_;
    }

    std::size_t request_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bodies_.size();
    }

    std::size_t connection_count() const {
        return connections_accepted_.load();
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        // Wake the blocking accept() by connecting to ourselves, so the serve
        // thread notices stopped_ and closes the acceptor from the thread that
        // owns it. Closing the acceptor from HERE races with accept() -- Asio
        // sockets are not thread-safe, and TSan flags it.
        try {
            boost::asio::io_context poke_context;
            tcp::socket poke(poke_context);
            poke.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));
            boost::system::error_code ignored;
            poke.close(ignored);
        } catch (const std::exception&) {
            // Already down; there is nothing to wake.
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    StubResponse next_response() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (script_.empty()) return StubResponse{};
        // Past the end of the script, keep using the last entry.
        const std::size_t index = std::min(served_, script_.size() - 1);
        ++served_;
        return script_[index];
    }

    void record(const std::string& body, const std::string& content_type,
                const std::string& idempotency_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        bodies_.push_back(body);
        content_types_.push_back(content_type);
        idempotency_keys_.push_back(idempotency_key);
    }

    void serve() {
        // One thread per accepted connection. The stub must not serialize
        // connections: it is the instrument for testing a CONCURRENT sink, and
        // a serializing instrument would make a concurrency test pass while
        // proving nothing.
        std::vector<std::thread> connections;
        while (true) {
            boost::system::error_code error;
            tcp::socket socket(io_context_);
            acceptor_.accept(socket, error);
            // Checked AFTER accept returns: stop() wakes us with a throwaway
            // connection, which must not be treated as a real request.
            if (error || stopped_.load()) break;
            connections_accepted_.fetch_add(1, std::memory_order_relaxed);
            connections.emplace_back(
                [this, socket = std::move(socket)]() mutable { handle(std::move(socket)); });
        }
        // Join before closing the acceptor so no handler outlives the server.
        for (auto& connection : connections) {
            if (connection.joinable()) connection.join();
        }
        // The acceptor is only ever closed by the thread that accepts on it.
        boost::system::error_code ignored;
        acceptor_.close(ignored);
    }

    void handle(tcp::socket socket) {
        // Loop: a real backend serves many requests over one keep-alive
        // connection, and the sink under test now relies on exactly that. A
        // stub that closed after every reply would defeat client-side
        // keep-alive regardless of what the sink does, so this keeps the
        // connection open across requests the same way, closing only when the
        // client closes it, the script says to hang up, or the response asks
        // for a close.
        beast::flat_buffer buffer;
        while (true) {
            boost::system::error_code error;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) return; // client closed or reset the connection

            auto content_type = request[http::field::content_type];
            auto idempotency_key = request["Idempotency-Key"];
            record(request.body(), std::string(content_type), std::string(idempotency_key));

            const StubResponse response = next_response();
            if (response.delay.count() > 0) {
                std::this_thread::sleep_for(response.delay);
            }
            if (response.action == StubAction::CloseWithoutReply) {
                socket.shutdown(tcp::socket::shutdown_both, error);
                return;
            }

            http::response<http::string_body> reply{status_for(response.action),
                                                    request.version()};
            reply.set(http::field::server, "edgeflow-stub");
            reply.set(http::field::content_type, "text/plain");
            if (response.action == StubAction::Redirect) {
                reply.set(http::field::location, "http://127.0.0.1:1/elsewhere");
            }
            reply.body() = "";
            reply.keep_alive(request.keep_alive());
            reply.prepare_payload();
            http::write(socket, reply, error);
            if (error || !request.keep_alive()) {
                socket.shutdown(tcp::socket::shutdown_both, error);
                return;
            }
        }
    }

    static http::status status_for(StubAction action) {
        switch (action) {
            case StubAction::ServerError:      return http::status::internal_server_error;
            case StubAction::TooManyRequests:  return http::status::too_many_requests;
            case StubAction::BadRequest:       return http::status::bad_request;
            case StubAction::Redirect:         return http::status::found;
            case StubAction::Ok:
            case StubAction::CloseWithoutReply:
            default:                           return http::status::ok;
        }
    }

    boost::asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::size_t> connections_accepted_{0};

    mutable std::mutex mutex_;
    std::vector<StubResponse> script_;
    std::size_t served_ = 0;
    std::vector<std::string> bodies_;
    std::vector<std::string> content_types_;
    std::vector<std::string> idempotency_keys_;
};

} // namespace edgeflow::testing
