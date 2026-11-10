#include "forge/lz4.hpp"

#include <lz4.h>
#include <lz4frame.h>

#include <cerrno>
#include <memory>
#include <stdexcept>

namespace forge::lz4 {
namespace {

Status error(int code, const char* what) { return Status::error(module_id::lz4, code, what); }

struct Context {
    LZ4F_dctx* ctx = nullptr;
    Context() {
        if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) throw std::bad_alloc();
    }
    ~Context() { LZ4F_freeDecompressionContext(ctx); }
};

}  // namespace

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data, Options options) {
    LZ4F_preferences_t prefs{};
    prefs.compressionLevel = options.level;
    prefs.frameInfo.contentSize = data.size();  // the reader knows the size in advance
    prefs.frameInfo.contentChecksumFlag = options.checksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
    std::vector<std::uint8_t> out(LZ4F_compressFrameBound(data.size(), &prefs));
    const auto n = LZ4F_compressFrame(out.data(), out.size(), data.data(), data.size(), &prefs);
    if (LZ4F_isError(n)) throw std::runtime_error(std::string("lz4: ") + LZ4F_getErrorName(n));
    out.resize(n);
    return out;
}

std::vector<std::uint8_t> compress(std::string_view text, Options options) {
    return compress(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), options);
}

Status decompress(std::span<const std::uint8_t> frame, std::vector<std::uint8_t>& out, std::size_t max_size) {
    if (frame.empty()) return error(EBADMSG, "no data");
    Context d;
    const auto start = out.size();
    std::size_t in = 0;
    std::size_t hint = 1;
    while (in < frame.size()) {
        const std::size_t produced = out.size() - start;
        std::size_t room = std::max<std::size_t>(64 * 1024, std::min<std::size_t>(produced, 4u * 1024 * 1024));
        if (produced + room > max_size) room = max_size - produced + 1;  // one more byte shows "too large"
        out.resize(start + produced + room);
        std::size_t dst = room;
        std::size_t src = frame.size() - in;
        hint = LZ4F_decompress(d.ctx, out.data() + start + produced, &dst, frame.data() + in, &src, nullptr);
        out.resize(start + produced + dst);
        if (LZ4F_isError(hint)) {
            out.resize(start);
            return error(EBADMSG, "not LZ4, or damaged");
        }
        if (out.size() - start > max_size) {
            out.resize(start);
            return error(EMSGSIZE, "larger than allowed");
        }
        in += src;
        if (src == 0 && dst == 0) break;  // no progress: the input is cut short
    }
    if (hint != 0) {  // the last frame did not end
        out.resize(start);
        return error(EBADMSG, "cut short");
    }
    return {};
}

const char* version() { return LZ4_versionString(); }

}  // namespace forge::lz4
