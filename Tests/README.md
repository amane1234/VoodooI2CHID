# Protocol tests

`VoodooI2CHID/VoodooI2CHIDProtocol.hpp` holds the I2C-HID wire handling of `VoodooI2CHIDDevice`: the HID descriptor layout and check, GET_REPORT/SET_REPORT command encoding, SET_REPORT framing, the length checks on input reports and GET_REPORT replies, and the polling interval policy. It has no IOKit dependency, so the kext and these host tests compile the same code.

## Unit tests

```bash
make -C Tests test
```

Runs under AddressSanitizer and UBSan. SET_REPORT and GET_REPORT framing were rewritten in 851fcb3; `Reference.hpp` keeps the implementation from before (d8e8bf6), and the tests require the same bytes on the wire for every report ID, report type and length, including the largest allowed report. Length checks are tested exhaustively for small sizes, and the polling interval against the 059886d policy.

## Fuzzing

```bash
make -C Tests run-fuzz FUZZ_SECONDS=300
```

| Target | Covers |
|---|---|
| `fuzz_input_read` | `getInputReport()` (length field, comparison with the previous report, payload copy), the GET_REPORT reply copy and the descriptor check |
| `fuzz_set_report` | SET_REPORT framing in an exact-size buffer, compared byte for byte with the d8e8bf6 reference and parsed back |

Device data goes into exact-size heap buffers, so a read or copy past what the length checks allowed is reported by ASan. Seeds are in `corpus/<target>`; new findings go to `build/corpus/<target>`. A crash writes `crash-*` in `Tests/`; reproduce it with `./build/<target> crash-...`.

libFuzzer needs clang with its runtime: on Linux `clang` and `libclang-rt-dev` (or the versioned package), on macOS Homebrew `llvm` (Apple's clang has no libFuzzer). The unit tests build with any C++17 compiler.

The GitHub Actions workflow `.github/workflows/tests.yml` runs both on every push.
