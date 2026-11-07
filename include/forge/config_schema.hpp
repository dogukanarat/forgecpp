#pragma once
// Declared configuration: types, ranges, choices and documentation of every
// key. Applications declare theirs in setup_config(); forgecpp declares its
// own ([log], [http], [forge], ...). At start-up (and on reload):
//   * a value of the wrong type, out of range or not one of the choices
//     stops the start with a message naming the key,
//   * an unknown key inside a declared section is reported as a warning
//     (catches typos such as "tap_rxx"),
//   * `--help-config` prints every key with type, default and description,
//     `--check-config` validates the file and exits.
//
//   void setup_config(forge::config::Schema& s) override {
//       s.section("radio", "AD936x transceiver");
//       s.string("radio.backend", "sim", "sim: simulator, iio: libiio device").choices({"sim", "iio"});
//       s.number("radio.sample_rate_hz", 30.72e6, "Sample rate").range(2.083334e6, 61.44e6).unit("Hz");
//       s.array("radio.rx_channels", json::array({1}), "RX channels it listens to");
//       s.open_section("auth.users", "One table per user");   // any key allowed
//   }

#include "forge/json.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forge {
class Config;
}

namespace forge::config {

enum class Type { boolean, integer, number, string, array, table, any };
const char* to_string(Type t);

class Key {
public:
    Key& doc(std::string text);
    Key& range(double min, double max);
    Key& choices(std::vector<std::string> values);
    Key& unit(std::string u);
    Key& required(bool r = true);
    /// Do not print the value (passwords, tokens).
    Key& secret(bool s = true);
    /// The default depends on the environment (e.g. colors on a terminal):
    /// `default_value` is documentation only, not filled in by apply_defaults().
    Key& computed(bool c = true);

    std::string path;
    Type type = Type::any;
    json default_value;
    std::string doc_text, unit_text;
    std::optional<double> min, max;
    std::vector<std::string> choice_values;
    bool is_required = false;
    bool is_secret = false;
    bool is_computed = false;
};

class Schema {
public:
    /// Declare a section (documentation; keys under it are checked).
    Schema& section(const std::string& path, std::string doc = {});
    /// A section whose keys are free-form (not checked), e.g. auth.users.
    Schema& open_section(const std::string& path, std::string doc = {});

    Key& key(const std::string& path, Type type, json default_value, std::string doc = {});
    Key& boolean(const std::string& path, bool def, std::string doc = {});
    Key& integer(const std::string& path, std::int64_t def, std::string doc = {});
    Key& number(const std::string& path, double def, std::string doc = {});
    Key& string(const std::string& path, std::string def, std::string doc = {});
    Key& array(const std::string& path, json def, std::string doc = {});
    Key& table(const std::string& path, std::string doc = {});

    const Key* find(const std::string& path) const;

    struct Result {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        bool ok() const { return errors.empty(); }
    };
    Result validate(const Config& cfg) const;

    /// Fill in the default of every declared key the configuration leaves out,
    /// so the schema is the only place a default is written. forge::Application
    /// does this after loading (and reloading): read declared keys with
    /// cfg.value<T>(key), not get_or(key, <default again>).
    void apply_defaults(Config& cfg) const;

    /// Human-readable reference of every key (TOML-like, grouped by section).
    std::string help() const;

private:
    struct Section {
        std::string doc;
        bool open = false;
    };
    bool covered_by_open_section(const std::string& path) const;
    std::map<std::string, Section> sections_;
    std::map<std::string, Key> keys_;
};

}  // namespace forge::config
