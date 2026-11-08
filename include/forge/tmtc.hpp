#pragma once
// tmtc: the commands a program answers, by number, as in zephyrus.
//
// A command is a request message, a reply message and a handler. The same
// handler answers binary requests (c2 over a link) and JSON (the web):
//
//   forge::tmtc::Registry commands;
//   commands.add<radio_tmtc_status_request, radio_tmtc_status_reply>(
//       RADIO_TMTC_ID(status), "radio_status",
//       [&](const radio_tmtc_status_request& req, radio_tmtc_status_reply& rsp) {
//           rsp.rx_lo_hz = radio.lo();
//           return forge::Status();
//       });
//
//   forge::tmtc::Dispatcher dispatcher(commands);
//   dispatcher.handle(received_bytes, [&](std::span<const std::uint8_t> answer) { link.send(answer); });
//
// The request and the reply live on the stack of the call: a binary request
// allocates nothing and throws nothing. A number or a name used twice is a
// mistake in the program, and add() throws std::logic_error at once.

#include "forge/c2.hpp"
#include "forge/json.hpp"
#include "forge/pb.hpp"
#include "forge/status.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::tmtc {

/// A handler that answers in parts sets `more` to a value it chooses (where
/// to go on) and is called again with it; it sets 0 on its last part.
/// Throws forge::Error with the HTTP status of `s` (the JSON path).
[[noreturn]] void throw_status(const Status& s);

template <class Req, class Rsp>
using PartHandler = std::function<Status(const Req& req, Rsp& rsp, std::uint32_t& more)>;
template <class Req, class Rsp>
using Handler = std::function<Status(const Req& req, Rsp& rsp)>;

struct Command {
    std::uint16_t id = 0;
    std::string name;
    const pb::Type* req = nullptr;  ///< nullptr for a push
    const pb::Type* rsp = nullptr;  ///< the message of a reply, or of a push

    bool is_push() const { return req == nullptr; }

    /// Decode the request, run the handler, encode one reply into `out`.
    std::function<Status(std::span<const std::uint8_t> req, std::span<std::uint8_t> out, std::size_t& written,
                         std::uint32_t& more)>
        run_binary;
    /// The same handler for JSON. Throws forge::Error (400 for a wrong request,
    /// the status of the handler otherwise). Parts come back as a JSON list.
    std::function<json(const json& req)> run_json;
};

class Registry {
public:
    template <pb::Message Req, pb::Message Rsp>
    void add(std::uint16_t id, std::string name, Handler<Req, Rsp> handler) {
        add_parts<Req, Rsp>(id, std::move(name),
                            [h = std::move(handler)](const Req& req, Rsp& rsp, std::uint32_t& more) {
                                more = 0;
                                return h(req, rsp);
                            });
    }

    template <pb::Message Req, pb::Message Rsp>
    void add_parts(std::uint16_t id, std::string name, PartHandler<Req, Rsp> handler);

    /// A message this program sends without a request (a report). A request
    /// for its number is refused with ENOSYS.
    template <pb::Message Msg>
    void add_push(std::uint16_t id, std::string name) {
        Command c;
        c.id = id;
        c.name = std::move(name);
        c.rsp = pb::type_of<Msg>;
        insert(std::move(c));
    }

    void remove(std::uint16_t id);

    const Command* find(std::uint16_t id) const;
    const Command* find(std::string_view name) const;
    /// Sorted by number.
    std::vector<const Command*> list() const;

private:
    void insert(Command c);

    std::map<std::uint16_t, std::unique_ptr<Command>> by_id_;
    std::map<std::string, Command*, std::less<>> by_name_;
};

/// Answers c2 requests from a Registry. Thread-safe: each call uses its own
/// buffer, so links can call handle() from several threads.
class Dispatcher {
public:
    using Send = std::function<void(std::span<const std::uint8_t> message)>;

    explicit Dispatcher(const Registry& registry) : registry_(registry) {}

    /// One received message. A request gets its answer through `send`: a
    /// reply, an ACK (an empty reply), a NACK with the status, or several
    /// replies with c2::flag::more. Other kinds are not answered; the result
    /// tells what the message was (c2::Kind::unknown if it did not decode).
    c2::Kind handle(std::span<const std::uint8_t> message, const Send& send) const;

private:
    const Registry& registry_;
};

// --- implementation of the templates -------------------------------------------------
template <pb::Message Req, pb::Message Rsp>
void Registry::add_parts(std::uint16_t id, std::string name, PartHandler<Req, Rsp> handler) {
    auto h = std::make_shared<PartHandler<Req, Rsp>>(std::move(handler));
    Command c;
    c.id = id;
    c.name = std::move(name);
    c.req = pb::type_of<Req>;
    c.rsp = pb::type_of<Rsp>;
    c.run_binary = [h](std::span<const std::uint8_t> in, std::span<std::uint8_t> out, std::size_t& written,
                       std::uint32_t& more) -> Status {
        Req req{};
        if (auto s = pb::decode(in, req); !s.ok()) return s;
        Rsp rsp{};
        if (auto s = (*h)(req, rsp, more); !s.ok()) {
            more = 0;
            return s;
        }
        return pb::encode(rsp, out, written);
    };
    c.run_json = [h](const json& in) -> json {
        const Req req = pb::from_json<Req>(in.is_null() ? json::object() : in);
        json parts = json::array();
        std::uint32_t more = 0;
        do {
            Rsp rsp{};
            if (auto s = (*h)(req, rsp, more); !s.ok()) throw_status(s);
            parts.push_back(pb::to_json(rsp));
        } while (more != 0);
        return parts.size() == 1 ? std::move(parts[0]) : parts;
    };
    insert(std::move(c));
}

}  // namespace forge::tmtc
