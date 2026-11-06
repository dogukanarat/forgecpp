#include <forge/http.hpp>
#include <forge/testing.hpp>

using forge::json;
using forge::http::HttpError;

TEST_CASE("http: json server and client") {
    forge::http::Server server;
    server.get_json("/api/ping", [](const auto&) { return json{{"pong", true}}; });
    server.get_json("/api/items/:id", [](const forge::http::Request& req) {
        return json{{"id", req.path_params.at("id")}, {"q", req.get_param_value("q")}};
    });
    server.post_json("/api/echo", [](const auto&, const json& body) {
        if (!body.contains("name")) throw HttpError(422, "name is required");
        return json{{"hello", body["name"]}};
    });
    server.del_json("/api/items/:id", [](const auto&) { return json(); });
    server.get_json("/api/crash", [](const auto&) -> json { throw std::runtime_error("kaboom"); });

    const int port = server.start("127.0.0.1", 0);
    REQUIRE(port > 0);
    CHECK(server.running());

    forge::http::Client client("http://127.0.0.1:" + std::to_string(port));
    client.timeout(std::chrono::seconds(2));

    CHECK(client.get_json("/api/ping")["pong"] == true);
    const auto item = client.get_json("/api/items/42", {{"q", "x"}});
    CHECK(item["id"] == "42");
    CHECK(item["q"] == "x");
    CHECK(client.post_json("/api/echo", {{"name", "bob"}})["hello"] == "bob");
    CHECK(client.del_json("/api/items/1").is_null());

    try {
        client.post_json("/api/echo", json::object());
        FAIL("expected HttpError");
    } catch (const HttpError& e) {
        CHECK(e.status() == 422);
        CHECK(e.body()["error"] == "name is required");
    }

    try {
        client.get_json("/api/crash");
        FAIL("expected HttpError");
    } catch (const HttpError& e) {
        CHECK(e.status() == 500);
    }

    try {
        client.get_json("/nope");
        FAIL("expected HttpError");
    } catch (const HttpError& e) {
        CHECK(e.status() == 404);
    }

    auto& raw = client.native();
    auto bad = raw.Post("/api/echo", "{not json", "application/json");
    REQUIRE(bad);
    CHECK(bad->status == 400);

    server.stop();
    CHECK_FALSE(server.running());

    forge::http::Client dead("http://127.0.0.1:" + std::to_string(port));
    dead.timeout(std::chrono::milliseconds(300));
    CHECK_THROWS_AS(dead.get_json("/api/ping"), HttpError);
}
