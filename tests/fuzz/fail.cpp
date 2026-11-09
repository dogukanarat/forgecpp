// fuzz_fail() for libFuzzer builds: an abort is what libFuzzer reports.
#include "fuzz.hpp"

#include <cstdio>
#include <cstdlib>

void fuzz_fail(const char* what) {
    std::fprintf(stderr, "fuzz: %s\n", what);
    std::abort();
}
