#include "systemd.hpp"

#include "proc.hpp"

#include <forge/hub.hpp>

#include <cctype>
#include <charconv>
#include <sstream>
#include <utility>

using namespace std::chrono_literals;

namespace forgehub {

std::uint64_t timespan_us(const std::string& s) {
    if (s.empty() || s == "infinity") return 0;
    static const std::pair<const char*, double> units[] = {
        {"usec", 1}, {"us", 1}, {"msec", 1e3}, {"ms", 1e3}, {"seconds", 1e6}, {"second", 1e6}, {"sec", 1e6},
        {"s", 1e6}, {"minutes", 60e6}, {"minute", 60e6}, {"min", 60e6}, {"hours", 3600e6}, {"hour", 3600e6},
        {"hr", 3600e6}, {"h", 3600e6}, {"days", 86400e6}, {"day", 86400e6}, {"d", 86400e6},
        {"weeks", 604800e6}, {"week", 604800e6}, {"w", 604800e6}};
    double total = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        std::size_t j = i;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
        if (j == i) return 0;
        const double n = std::stod(s.substr(i, j - i));
        std::size_t k = j;
        while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) ++k;
        const auto unit = s.substr(j, k - j);
        double scale = unit.empty() ? 1 : 0;
        for (const auto& [name, f] : units) {
            if (unit == name) scale = f;
        }
        if (scale == 0) return 0;
        total += n * scale;
        any = true;
        i = k;
    }
    return any ? static_cast<std::uint64_t>(total) : 0;
}

namespace {

// systemctl's numbers; 0 for "", "infinity" and anything else that is not one.
std::uint64_t to_u64(const std::string& s) {
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && end == s.data() + s.size() ? v : 0;
}

class Systemctl final : public Systemd {
public:
    std::string unit_of(int pid) override {
        const auto unit = forge::hub::unit_of(pid);
        if (!valid_unit_name(unit)) return {};
        // Only the unit's main process counts (not, e.g., a child of a CI runner's service).
        const auto r = run_command({"systemctl", "show", "-p", "MainPID", "--value", unit}, 3s);
        return r.exit_code == 0 && r.output == std::to_string(pid) ? unit : std::string();
    }

    std::map<std::string, UnitState> show(const std::vector<std::string>& units) override {
        std::vector<std::string> argv{"systemctl", "show",
                                      "-p", "Id,LoadState,ActiveState,SubState,Result,MainPID,NRestarts,Restart,"
                                            "StartLimitBurst,StartLimitIntervalUSec,RestartUSec,WatchdogUSec"};
        for (const auto& u : units) {
            if (valid_unit_name(u)) argv.push_back(u);
        }
        if (argv.size() == 4) return {};
        const auto r = run_command(argv, 5s);
        if (r.exit_code != 0) return {};
        return parse_show(r.output);
    }

    bool action(const std::string& verb, const std::string& unit, std::string* error) override {
        if (!valid_unit_name(unit)) {
            if (error) *error = "invalid unit name '" + unit + "'";
            return false;
        }
        const auto r = run_command({"systemctl", verb, unit}, 30s);
        if (r.exit_code == 0) return true;
        if (error) *error = "systemctl " + verb + " " + unit + ": " + (r.output.empty() ? "failed" : r.output);
        return false;
    }
};

}  // namespace

std::unique_ptr<Systemd> make_systemctl() { return std::make_unique<Systemctl>(); }

std::map<std::string, UnitState> parse_show(const std::string& output) {
    std::map<std::string, UnitState> out;
    std::istringstream in(output + "\n\n");
    std::string line, id;
    UnitState st;
    auto flush = [&] {
        if (!id.empty()) out[id] = st;
        id.clear();
        st = UnitState{};
    };
    while (std::getline(in, line)) {
        if (line.empty()) {
            flush();
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "Id") id = value;
        else if (key == "LoadState") st.loaded = value == "loaded";
        else if (key == "ActiveState") st.active = value;
        else if (key == "SubState") st.sub = value;
        else if (key == "Result") st.result = value;
        else if (key == "MainPID") st.main_pid = static_cast<int>(to_u64(value));
        else if (key == "NRestarts") st.n_restarts = static_cast<std::uint32_t>(to_u64(value));
        else if (key == "Restart") st.restart = value;
        else if (key == "StartLimitBurst") st.start_limit_burst = static_cast<std::uint32_t>(to_u64(value));
        else if (key == "StartLimitIntervalUSec") st.start_limit_interval_us = timespan_us(value);
        else if (key == "RestartUSec") st.restart_us = timespan_us(value);
        else if (key == "WatchdogUSec") st.watchdog_us = timespan_us(value);
    }
    return out;
}

}  // namespace forgehub
