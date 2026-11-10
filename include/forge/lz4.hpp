#pragma once
// LZ4 compression (forgecpp::lz4, libforgecpp-lz4) in the LZ4 frame format:
// what the lz4 command line tool and Python's lz4.frame read and write, so a
// file or a message compressed here opens anywhere (`lz4 -d batch.lz4`).
//
//   auto packed = forge::lz4::compress(bytes);               // fast: hundreds of MB/s on a Cortex-A
//   std::vector<std::uint8_t> out;
//   if (auto s = forge::lz4::decompress(packed, out); !s.ok()) ...   // EBADMSG, EMSGSIZE
//
// Decompression is given a limit: data from a link or a file must not grow
// into all the memory there is.

#include "forge/status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace forge::lz4 {

struct Options {
    int level = 0;          ///< 0: fast (the default); 3..12: smaller and slower (LZ4 HC)
    bool checksum = true;   ///< a content checksum: damaged data is found
};

/// One LZ4 frame holding `data`.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data, Options options = {});
std::vector<std::uint8_t> compress(std::string_view text, Options options = {});

/// The data of one or more LZ4 frames, appended to `out`. EBADMSG: not LZ4
/// or damaged (a checksum that does not match); EMSGSIZE: more than
/// `max_size` bytes (nothing is appended then).
Status decompress(std::span<const std::uint8_t> frame, std::vector<std::uint8_t>& out,
                  std::size_t max_size = 64u * 1024 * 1024);

/// The LZ4 library this program runs with ("1.9.4").
const char* version();

}  // namespace forge::lz4
