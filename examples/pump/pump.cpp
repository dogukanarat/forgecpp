#include "pump.hpp"

#include <cstdlib>
#include <fstream>

std::string Pump::write_csv(const std::vector<double>& h) {
    const char* dir = std::getenv("STATE_DIRECTORY");
    const std::string file = std::string(dir ? dir : "/tmp") + "/pump-history.csv";
    std::ofstream out(file);
    out << "sample,pressure_bar\n";
    for (std::size_t i = 0; i < h.size(); ++i) out << i << ',' << h[i] << '\n';
    return file;
}
