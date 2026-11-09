#include "forge/json.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace forge {

json load_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return json::parse(in);
}

void save_json(const std::filesystem::path& path, const json& value, int indent) {
    write_file_atomic(path, value.dump(indent) + "\n");
}

void write_file_atomic(const std::filesystem::path& path, std::string_view content) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());

    const auto tmp = path.string() + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("cannot create " + tmp + ": " + std::strerror(errno));

    const char* data = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("cannot write " + tmp + ": " + std::strerror(err));
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error("cannot rename " + tmp + ": " + std::strerror(err));
    }
}

}  // namespace forge
