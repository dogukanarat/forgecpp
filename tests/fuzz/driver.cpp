// Runs a fuzz target without libFuzzer: the seeds, then mutations of them.
// Deterministic (fixed random seed), so a failure can be reproduced.
//
//   fuzz_c2                    # FORGE_FUZZ_RUNS (default 20000) mutations
//   fuzz_c2 file...            # these inputs only (fuzz-failure.bin, a libFuzzer crash file, ...)
#include "fuzz.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

const std::string* current = nullptr;  // the input being run, saved when it fails

void run(const std::string& input) {
    current = &input;
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

std::string mutate(std::string s, std::mt19937& rng) {
    static const std::uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x40, 0x20, 0x08};
    auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
    const std::size_t steps = 1 + pick(6);
    for (std::size_t i = 0; i < steps; ++i) {
        switch (pick(6)) {
            case 0:  // flip a bit
                if (!s.empty()) s[pick(s.size())] ^= static_cast<char>(1u << pick(8));
                break;
            case 1:  // an interesting byte
                if (!s.empty()) s[pick(s.size())] = static_cast<char>(interesting[pick(std::size(interesting))]);
                break;
            case 2:  // insert a byte
                s.insert(s.begin() + static_cast<std::ptrdiff_t>(pick(s.size() + 1)), static_cast<char>(pick(256)));
                break;
            case 3:  // delete a byte
                if (!s.empty()) s.erase(pick(s.size()), 1);
                break;
            case 4:  // cut the end off
                if (!s.empty()) s.resize(pick(s.size()));
                break;
            default:  // repeat a piece
                if (!s.empty()) {
                    const auto at = pick(s.size());
                    s.insert(pick(s.size() + 1), s.substr(at, 1 + pick(s.size() - at)));
                }
        }
    }
    return s;
}

}  // namespace

void fuzz_fail(const char* what) {
    std::fprintf(stderr, "fuzz: %s\n", what);
    if (current) {
        std::ofstream("fuzz-failure.bin", std::ios::binary)
            .write(current->data(), static_cast<std::streamsize>(current->size()));
        std::fprintf(stderr, "fuzz: input saved in fuzz-failure.bin (run again: <target> fuzz-failure.bin)\n");
    }
    std::abort();
}

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary);
            run(std::string(std::istreambuf_iterator<char>(in), {}));
        }
        return 0;
    }
    const char* env = std::getenv("FORGE_FUZZ_RUNS");
    const long runs = env ? std::atol(env) : 20000;
    std::mt19937 rng(20261109);
    const auto& base = seeds();
    for (const auto& s : base) run(s);
    for (long i = 0; i < runs; ++i) {
        if (i % 10 == 0) {  // now and then something without a seed
            std::string r(std::uniform_int_distribution<int>(0, 64)(rng), '\0');
            for (auto& c : r) c = static_cast<char>(rng());
            run(r);
        } else {
            run(mutate(base[i % base.size()], rng));
        }
    }
    std::printf("fuzz: %zu seeds, %ld inputs, no failure\n", base.size(), runs);
    return 0;
}
