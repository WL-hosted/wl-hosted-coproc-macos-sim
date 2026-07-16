# WL-hosted Coprocessor macOS Simulator

POSIX adapter for `wlh_coproc_core` with exact simulator IPC v1 framing,
runtime/fault sideband, Ethernet echo, and deterministic Open/WPA2/WPA3 mock
networks.

The POSIX adapter runs independent Core, TX, and mock Wi-Fi tasks through the
same opaque OSAL contract intended for MCU adapters. Mock Wi-Fi operations are
accepted immediately but complete after a deterministic delay, with scan and
link results returned through asynchronous Core event ingress. The scenario
main loop never polls the Core.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/wlh-coproc-macos-sim --ipc connect:/tmp/coproc.sock --scenario happy
```
