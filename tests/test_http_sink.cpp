#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include "stub_http_server.hpp"

using edgeflow::testing::StubAction;
using edgeflow::testing::StubHttpServer;
using edgeflow::testing::StubResponse;

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// Minimal one-shot client, so the stub's tests do not depend on HttpSink.
// Returns the status code, or 0 if the server hung up without replying.
unsigned post(std::uint16_t port, const std::string& body) {
    boost::asio::io_context io_context;
    tcp::socket socket(io_context);
    socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

    http::request<http::string_body> request{http::verb::post, "/batches", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/x-ndjson");
    request.body() = body;
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code error;
    http::read(socket, buffer, response, error);
    if (error) return 0;
    return response.result_int();
}

} // namespace

TEST(StubHttpServer, RepliesOkAndRecordsTheBody) {
    StubHttpServer server;
    server.script({{StubAction::Ok, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "hello\n"), 200u);
    ASSERT_EQ(server.request_count(), 1u);
    EXPECT_EQ(server.bodies().at(0), "hello\n");
    EXPECT_EQ(server.content_types().at(0), "application/x-ndjson");
}

TEST(StubHttpServer, FollowsTheScriptInOrder) {
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)},
                   {StubAction::TooManyRequests, std::chrono::milliseconds(0)},
                   {StubAction::Ok, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 500u);
    EXPECT_EQ(post(server.port(), "b\n"), 429u);
    EXPECT_EQ(post(server.port(), "c\n"), 200u);
}

TEST(StubHttpServer, RepeatsTheLastEntryPastTheEndOfTheScript) {
    // So "always fail" needs only a one-entry script.
    StubHttpServer server;
    server.script({{StubAction::ServerError, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 500u);
    EXPECT_EQ(post(server.port(), "b\n"), 500u);
    EXPECT_EQ(post(server.port(), "c\n"), 500u);
}

TEST(StubHttpServer, CloseWithoutReplyLooksLikeATransportFailure) {
    StubHttpServer server;
    server.script({{StubAction::CloseWithoutReply, std::chrono::milliseconds(0)}});

    EXPECT_EQ(post(server.port(), "a\n"), 0u) << "expected no readable response";
    EXPECT_EQ(server.request_count(), 1u) << "the request should still have been read";
}
