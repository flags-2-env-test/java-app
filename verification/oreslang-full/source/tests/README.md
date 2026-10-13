# LLVM compiler contract fixtures

Run `ctest --test-dir build --output-on-failure`.
Positive tests verify generated IR and optionally compile and execute IR with Clang;
negative tests require specific diagnostics, failure status, and no IR on stdout.
These are bootstrap compiler contracts, not full differential parity with Java.
