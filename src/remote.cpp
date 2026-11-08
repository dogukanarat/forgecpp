#include "forge/remote.hpp"

#include "forge/error.hpp"
#include "forge/messages.forge.hpp"

namespace forge {
namespace {

void check(const Status& s, const std::string& id) {
    if (!s.ok()) throw Error(s.http_status(), id + ": " + s.to_string());
}

template <std::size_t N>
void put(char (&dst)[N], const std::string& text, const std::string& id) {
    if (!pb::assign(dst, text).ok()) throw Error(400, id + ": too long for the link (max " + std::to_string(N - 1) + ")");
}

json parse(const char* text) { return text[0] == '\0' ? json() : json::parse(text); }

}  // namespace

Remote::Remote(std::string endpoint, std::chrono::milliseconds timeout) : client_(std::move(endpoint), timeout) {}

json Remote::get(const std::string& id) {
    forge_get_request req{};
    put(req.id, id, id);
    forge_get_reply rsp{};
    check(client_.call(forge_msg_id_get, req, rsp), id);
    return parse(rsp.value);
}

json Remote::set(const std::string& id, const json& value) {
    forge_set_request req{};
    put(req.id, id, id);
    put(req.value, value.dump(), id);
    forge_set_reply rsp{};
    check(client_.call(forge_msg_id_set, req, rsp), id);
    return parse(rsp.value);
}

void Remote::press(const std::string& id) {
    forge_press_request req{};
    put(req.id, id, id);
    forge_press_reply rsp{};
    check(client_.call(forge_msg_id_press, req, rsp), id);
}

json Remote::call(const std::string& id, const json& args) {
    forge_call_request req{};
    put(req.id, id, id);
    put(req.args, args.dump(), id);
    forge_call_reply rsp{};
    check(client_.call(forge_msg_id_call, req, rsp), id);
    return parse(rsp.result);
}

}  // namespace forge
