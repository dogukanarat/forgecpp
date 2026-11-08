#pragma once
// The controls of another forgecpp program, through its [forge].control
// endpoint: what forgectl get/set/press/call do, for code.
//
//   forge::Remote radio("tcp://127.0.0.1:5555");
//   radio.set("rx_lo_mhz", 2412);
//   const double rate = radio.get("sample_rate_msps");
//
// Values are JSON, like on the web panel. A refusal throws forge::Error with
// the HTTP-like status of the reason (400 out of range, 403 role, 404 unknown
// id, 504 no answer). The binary link only carries the status code, not the
// text of the reason.

#include "forge/json.hpp"
#include "forge/zmq.hpp"

#include <chrono>
#include <string>

namespace forge {

class Remote {
public:
    explicit Remote(std::string endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(3));

    json get(const std::string& id);
    /// Returns the value the control took (it may round or clamp).
    json set(const std::string& id, const json& value);
    void press(const std::string& id);
    json call(const std::string& id, const json& args = json::object());

    zmq::Client& client() { return client_; }

private:
    zmq::Client client_;
};

}  // namespace forge
