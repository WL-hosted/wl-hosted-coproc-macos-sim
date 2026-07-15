# WL-hosted Coprocessor macOS Simulator

POSIX adapter for `wlh_coproc_core` with exact simulator IPC v1 framing,
runtime/fault sideband, Ethernet echo, and deterministic Open/WPA2/WPA3 mock
networks.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/wlh-coproc-macos-sim --ipc connect:/tmp/coproc.sock --scenario happy
```
