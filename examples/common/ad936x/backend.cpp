#include "backend.hpp"

#include <algorithm>

namespace radio {

nlohmann::json to_json(const Settings& s) {
    return {{"rx_lo_hz", s.rx_lo_hz},
            {"tx_lo_hz", s.tx_lo_hz},
            {"sample_rate_hz", s.sample_rate_hz},
            {"rx_bandwidth_hz", s.rx_bandwidth_hz},
            {"tx_bandwidth_hz", s.tx_bandwidth_hz},
            {"gain_mode", s.gain_mode},
            {"rx_gain_db", s.rx_gain_db},
            {"tx_attenuation_db", s.tx_attenuation_db},
            {"rx2_gain_mode", s.rx2_gain_mode},
            {"rx2_gain_db", s.rx2_gain_db}};
}

Settings settings_from_json(const nlohmann::json& j, Settings s) {
    if (!j.is_object()) return s;
    auto num = [&](const char* key, double& out, double lo, double hi) {
        if (j.contains(key) && j[key].is_number()) out = std::clamp(j[key].get<double>(), lo, hi);
    };
    num("rx_lo_hz", s.rx_lo_hz, Limits::lo_min_hz, Limits::lo_max_hz);
    num("tx_lo_hz", s.tx_lo_hz, Limits::lo_min_hz, Limits::lo_max_hz);
    num("sample_rate_hz", s.sample_rate_hz, Limits::rate_min_hz, Limits::rate_max_hz);
    num("rx_bandwidth_hz", s.rx_bandwidth_hz, Limits::bw_min_hz, Limits::bw_max_hz);
    num("tx_bandwidth_hz", s.tx_bandwidth_hz, Limits::bw_min_hz, Limits::bw_max_hz);
    num("rx_gain_db", s.rx_gain_db, Limits::rx_gain_min_db, Limits::rx_gain_max_db);
    num("tx_attenuation_db", s.tx_attenuation_db, Limits::tx_atten_min_db, Limits::tx_atten_max_db);
    num("rx2_gain_db", s.rx2_gain_db, Limits::rx_gain_min_db, Limits::rx_gain_max_db);
    auto mode = [&](const char* key, std::string& out) {
        if (!j.contains(key) || !j[key].is_string()) return;
        const auto m = j[key].get<std::string>();
        if (std::find(gain_modes().begin(), gain_modes().end(), m) != gain_modes().end()) out = m;
    };
    mode("gain_mode", s.gain_mode);
    mode("rx2_gain_mode", s.rx2_gain_mode);
    return s;
}

void Backend::apply(const Settings& s) {
    // Rate before bandwidth (the driver derives filters from the rate),
    // gain mode before manual gain.
    set_sample_rate(s.sample_rate_hz);
    set_rx_bandwidth(s.rx_bandwidth_hz);
    set_tx_bandwidth(s.tx_bandwidth_hz);
    set_rx_lo(s.rx_lo_hz);
    set_tx_lo(s.tx_lo_hz);
    set_gain_mode(s.gain_mode, 0);
    if (s.gain_mode == "manual") set_rx_gain(s.rx_gain_db, 0);
    if (rx_channel_count() > 1) {
        set_gain_mode(s.rx2_gain_mode, 1);
        if (s.rx2_gain_mode == "manual") set_rx_gain(s.rx2_gain_db, 1);
    }
    set_tx_attenuation(s.tx_attenuation_db);
}

#ifndef RADIO_HAS_IIO
std::unique_ptr<Backend> make_iio_backend(const std::string&, unsigned) {
    throw DeviceError("radio-controller was built without libiio support");
}
bool iio_supported() { return false; }
#endif

}  // namespace radio
