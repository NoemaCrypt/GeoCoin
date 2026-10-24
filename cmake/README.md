# CMake layout

The root `CMakeLists.txt` is an ordered entry point. Configuration is split by
responsibility while retaining the root directory scope:

- `Options.cmake` — build options, hardening, sanitizer and platform settings;
- `Dependencies.cmake` — OpenSSL, RocksDB and liboqs;
- `Libraries.cmake` — production sources, include boundaries and target links;
- `Tools.cmake` / `GpuMiner.cmake` — node, miner and diagnostic executables;
- `Tests.cmake` — common test setup;
- `tests/Headers.cmake` — standalone public-header compilation;
- `tests/Unit.cmake` — deterministic and unit executables;
- `tests/Integration.cmake` — process and local-network scenarios;
- `Fuzzers.cmake` — optional generated-input test executables.

Add source files to the narrowest production target. Tests may include private
headers through their dedicated include path; production targets may only use
another module's public headers. `scripts/check_architecture.py` enforces this
rule during every test build.
