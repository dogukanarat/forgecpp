#pragma once
// Fuzz targets for the code that reads what arrives on a link.
//
// Each target defines LLVMFuzzerTestOneInput() and seeds(). With libFuzzer
// (-DFORGECPP_FUZZ=ON and a clang that has it) the target is a libFuzzer
// binary; otherwise driver.cpp turns it into a short, repeatable test for
// ctest that mutates the seeds.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// Inputs that reach deep into the target (valid frames, valid messages).
const std::vector<std::string>& seeds();

/// A broken property: report it the same way libFuzzer reports a crash.
[[noreturn]] void fuzz_fail(const char* what);
