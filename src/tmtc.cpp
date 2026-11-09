#include "forge/tmtc.hpp"

#include "forge/error.hpp"
#include "forge/log.hpp"

#include <array>
#include <cerrno>
#include <format>
#include <stdexcept>

namespace forge::tmtc {

void throw_status(const Status& s) { throw Error(s.http_status(), s.to_string()); }

Status guarded(const std::function<Status()>& handler) {
    try {
        return handler();
    } catch (const Error& e) {
        log::debug("tmtc: refused: {}", e.what());
        return Status::error(module_id::tmtc, errno_for_http(e.status()), "refused");
    } catch (const std::exception& e) {
        log::error("tmtc: a command failed: {}", e.what());
        return Status::error(module_id::tmtc, EIO, "the command failed");
    }
}

void Registry::insert(Command c) {
    if (c.id == 0) throw std::logic_error("tmtc: command number 0 is not a command");
    if (by_id_.contains(c.id)) {
        throw std::logic_error(std::format("tmtc: number 0x{:04x} is already '{}'", c.id, by_id_[c.id]->name));
    }
    if (by_name_.contains(c.name)) throw std::logic_error("tmtc: name '" + c.name + "' is already used");
    auto owned = std::make_unique<Command>(std::move(c));
    by_name_[owned->name] = owned.get();
    by_id_[owned->id] = std::move(owned);
}

void Registry::remove(std::uint16_t id) {
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) return;
    by_name_.erase(it->second->name);
    by_id_.erase(it);
}

const Command* Registry::find(std::uint16_t id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second.get();
}

const Command* Registry::find(std::string_view name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

std::vector<const Command*> Registry::list() const {
    std::vector<const Command*> out;
    out.reserve(by_id_.size());
    for (const auto& [id, c] : by_id_) out.push_back(c.get());
    return out;
}

c2::Kind Dispatcher::handle(std::span<const std::uint8_t> message, const Send& send) const {
    c2::Message m;
    if (!c2::decode_message(message, m).ok()) return c2::Kind::unknown;
    const auto kind = c2::kind_of(m.header.flags);
    if (kind != c2::Kind::request) return kind;

    // One buffer per thread: a whole c2 message, header in front.
    thread_local std::array<std::uint8_t, c2::header_size + c2::payload_max> buf;
    const auto answer = [&](std::uint8_t flags, std::span<const std::uint8_t> payload) {
        std::size_t n = 0;
        const auto hdr =
            c2::make_header(flags, m.header.cmd_id, m.header.seq, static_cast<std::uint16_t>(payload.size()));
        if (c2::encode_message(hdr, payload, buf, n).ok()) send(std::span(buf).first(n));
    };
    const auto refuse = [&](const Status& s) { answer(c2::flag::nack, c2::nack_payload(s)); };

    const Command* cmd = registry_.find(m.header.cmd_id);
    if (cmd == nullptr) {
        refuse(Status::error(module_id::tmtc, ENOENT, "no such command"));
        return kind;
    }
    if (cmd->is_push()) {
        refuse(Status::error(module_id::tmtc, ENOSYS, "a report, not a command"));
        return kind;
    }

    std::uint32_t more = 0;
    do {
        std::size_t len = 0;
        // The payload is written in place, after the header.
        const auto s = cmd->run_binary(m.payload, std::span(buf).subspan(c2::header_size), len, more);
        if (!s.ok()) {
            refuse(s);
            break;
        }
        const std::uint8_t flags = more != 0  ? (c2::flag::rsp | c2::flag::more)
                                   : len == 0 ? c2::flag::ack
                                              : c2::flag::rsp;
        answer(flags, std::span(buf).subspan(c2::header_size, len));
    } while (more != 0);
    return kind;
}

}  // namespace forge::tmtc
