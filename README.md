# WL-hosted Coprocessor macOS Simulator

`wl-hosted-coproc-macos-sim` 是 WL-hosted 协处理器（Coprocessor）的 macOS / POSIX 模拟器。它基于嵌套的 `wl-hosted-coproc-core` 构建，在桌面端模拟一个带 Wi-Fi 的协处理器，用于验证标准协议栈、Wi-Fi 状态机以及与 Host 模拟器或 Sim Manager 的端到端交互。

本仓库不是生产固件，仅用于开发与 CI 测试；所有与真实 MCU 相关的时序、队列、缓冲区约束都通过 `wlh::osal` 抽象层表达，因此它也是 Core OSAL 可移植性的一个集成验证点。

## 1. 仓库定位

在 WL-hosted 多仓库工作区中，各仓库的职责边界如下：

```text
wl-hosted-coproc-macos-sim -> wl-hosted-coproc-core -> wl-hosted-protocol
                                                       -> wl-hosted-common
```

- `wl-hosted-coproc-core`：平台无关的 Coproc Core，负责标准 WL-hosted v1 协议状态机（Hello、Session、Credit、Channel、Heartbeat 等）。
- `wl-hosted-protocol`：标准 Wire/RPC codec 以及 Simulator IPC sideband protobuf（`sim_sideband.pb.h/.c`）。
- `wl-hosted-common`：共享平台契约，OSAL 唯一来源位于 `osal/include/wlh/osal.h`；本仓库显式启用 `wlh::posix_osal`。

本仓库的角色固定为 `SIM_ROLE_COPROC`。当对端是 Manager 且双方都声明 `SIDEBAND` 时，会启用 sideband 运行时/故障注入通道；与 Host Sim 直连时同样根据 Hello 中的 sideband 标志决定是否启用。

> Simulator IPC（v1）是测试专用通道，不属于标准 WL-hosted v1 协议，也不会出现在真实 ESP32-S3 固件上。

## 2. 构建要求

- macOS（开发测试目标平台）
- CMake >= 3.20
- C11 编译器（Clang 或 GCC）
- pthread

无需额外第三方库。

## 3. 构建步骤

推荐 out-of-tree 构建，并打开测试：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

如需 Release 构建：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

公共接口、OSAL 或生命周期相关改动，还应额外运行 sanitizer 构建：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

编译选项保持 `-Wall -Wextra -Wpedantic -Werror`，错误会被视为构建失败。

## 4. 运行方式

构建产物为 `build-debug/wlh-coproc-macos-sim`（或对应构建目录）。程序仅支持 IPC 模拟后端，通过 Unix domain socket 与对端通信。

### 4.1 直接连接 Host 模拟器

先启动 Host 模拟器监听 socket（参见 `wl-hosted-host-macos-sim` README），再启动本程序：

```sh
./build-debug/wlh-coproc-macos-sim \
    --ipc connect:/tmp/wlh-host.sock \
    --scenario happy
```

### 4.2 由 Sim Manager 拉起

Manager 会为本进程传入 `--manager-socket <path>`（内部被转换为 `connect:<path>`），并协商启用 sideband：

```sh
# 通常不需要手工调用；由 wl-hosted-macos-sim-manager 启动
./build-debug/wlh-coproc-macos-sim \
    --manager-socket /tmp/wlh-manager-coproc.sock \
    --scenario happy \
    --monitor-interval-ms 1000
```

## 5. 命令行参数

```text
--ipc connect:PATH|listen:PATH|fd:N
    指定 IPC 对端地址。connect 主动连接，listen 在指定路径监听并接受第一个连接，fd 使用已继承的文件描述符。

--manager-socket PATH
    Manager 启动时使用，等价于 --ipc connect:PATH，语义上标识对端是 Manager。

--scenario happy|auth-fail|ap-not-found
    指定 mock Wi-Fi 场景，默认 happy。

--monitor-interval-ms N
    设置 sideband 运行时信息上报间隔，单位毫秒，默认 1000，范围 1..60000。
```

## 6. 内置场景与 mock 网络

本程序模拟一个带 Wi-Fi 的协处理器，具备以下行为：

- **以太网回显**：收到 Ethernet 数据帧后，立即调用 `wlh_coproc_ethernet_sta_send()` 将其原样发回 Host，便于在不依赖真实 AP 的情况下做连通性测试。
- **mock Wi-Fi**：初始化、扫描、连接、断开均异步完成；操作被提交到独立的 `wifi_worker` 任务，经过固定时延后通过 Core event ingress 上报结果。
- **固定热点**：扫描永远返回三个 BSS。
- **可切换场景**：`happy`（正常连接）、`auth-fail`（任何连接都认证失败）、`ap-not-found`（任何连接都报告 AP 不存在）。

扫描结果固定返回以下三个 BSS：

| SSID | BSSID | Security | Channel | RSSI | 正确凭据 |
|------|-------|----------|---------|------|----------|
| `OpenLab` | `02:00:00:00:00:01` | Open (`1`) | 1 | -35 | 无 |
| `WPA2Net` | `02:00:00:00:00:02` | WPA2-PSK (`4`) | 6 | -48 | `password123` |
| `WPA3Net` | `02:00:00:00:00:03` | WPA3-SAE (`6`) | 36 | -57 | `sae-secret` |

在 `happy` 场景下，只有 SSID + security + 凭据完全匹配才会上报 `connected`；否则按具体错误上报 `disconnected`。`auth-fail` 场景下所有连接都会失败，`ap-not-found` 场景下所有连接都会因找不到 AP 而失败。

## 7. 架构与线程模型

本仓库把 Coproc Core 所需的 OSAL、buffer、port、wifi 四类操作以 C 结构体回调形式注入：

- **OSAL**：启用 Common 提供的 `wlh_posix_osal`，包含 pthread 任务、条件变量等待、有界队列和单调时钟。
- **Buffer**：由主程序分配/释放，支持通过故障注入模拟分配失败。
- **Port**：`transport_submit()` 将待发送帧投递到有界 TX 队列；`ethernet_rx()` 实现以太网回显。
- **Wi-Fi**：`wifi_initialize/scan/connect/disconnect` 将请求投递到有界 Wi-Fi 队列，由 `wifi_worker` 异步执行。

线程分布：

- **主线程**：阻塞在 `poll()` 上等待 socket 可读或下一次 telemetry 超时；收到标准帧后调用 `wlh_coproc_on_frame()`，收到 fault request 后调用 `handle_fault()`。不会周期性 poll Core。
- **TX 任务（`tx_worker`）**：Core 通过 `transport_submit()` 将待发送帧投递到深度 16 的有界队列；TX 任务负责调用 `wlh_sim_write_record()` 写入 socket，并回调 Core 的完成函数。
- **mock Wi-Fi 任务（`wifi_worker`）**：初始化、扫描、连接、断开请求通过深度 16 的有界队列序列化执行；每个操作先 `sleep_ms(backend_delay_ms)`，默认 25 ms，再通过 Core API 异步上报结果。

所有任务共享同一个 `simulator_t` 实例，但只通过 Core API 和 OSAL 原语交互；标准帧和 sideband 帧不消耗标准 channel credit，也不修改标准 Session/Sequence。

## 8. Sideband 运行时信息

当对端是 Manager 且双方都启用 `SIDEBAND` 时，本进程会周期性通过 `WLH_SIM_RECORD_RUNTIME_INFO` 上报：

- 当前角色（固定 `SIM_ROLE_COPROCESSOR`）
- 链路状态（up / down）
- session_id
- 运行时长
- TX/RX 帧数
- 空闲缓冲区数
- 实现名称与版本

这些信息仅用于测试/监控，不属于标准 WL-hosted wire 协议。

## 9. 故障注入

IPC + Manager 模式下，本进程可接收 `WLH_SIM_RECORD_FAULT_REQUEST` 并回复 `WLH_SIM_RECORD_FAULT_RESPONSE`。当前支持的 fault kind：

| Fault | 作用 |
|-------|------|
| `SESSION_CHANGE` / `PEER_RESET` | 调用 `wlh_coproc_test_reset_session()` 重置 session。 |
| `CLEAR_CREDIT` | 将 channel 1 和 2 的 credit 清 0。 |
| `CHANNEL_RESET` | 按 `channel` 字段重置指定 channel。 |
| `LIMIT_CREDIT` | 将指定 channel 的 credit 限制为 `count`。 |
| `BUFFER_OOM` | 让接下来 `count` 次 buffer 分配返回 NULL，模拟内存耗尽。 |
| `WIFI_DISCONNECT` | 上报一次非本地触发的断开。 |
| `WIFI_SCAN_FAILURE` | 让后续扫描直接失败。 |

Manager 负责路径级故障（丢包、重复、乱序、延迟、截断等）；上述 Kind 用于进程内部状态注入。

## 10. 测试

仓库自带的 CTest 包括：

- `wlh_sim_ipc`：`tests/test_ipc.c` 使用 `socketpair` 验证 Record 分包读取、Hello 编解码、畸形帧拒绝。

运行：

```sh
ctest --test-dir build-debug --output-on-failure
```

若修改了 Core、OSAL 或标准协议相关逻辑，应同时配合 `wl-hosted-host-macos-sim` 和 `wl-hosted-macos-sim-manager` 做端到端验收：READY、扫描三个 BSS、WPA2 连接、以太网 echo、断开与自动恢复。

## 11. 格式化

本仓库遵循工作区统一的 `.clang-format`。不要手动格式化，应从工作区根目录运行：

```sh
./auto_format.sh
```

该脚本会格式化 Protocol、Common、两个 Core、两个 Sim 和 Manager 中的 C/C++ 文件，并排除 submodule、`third_party`、生成的 `*.pb.*`、构建目录和 Rust `target`。格式化后建议重新构建受影响目标并再次运行脚本确认幂等。

## 12. 子模块与锁文件

本目录是独立 Git 仓库。修改后应单独提交，不要在工作区根目录执行全局 `git` 操作。

本仓库依赖的 `coproc-core` 子模块信息记录在：

- `.gitmodules`
- `coproc-core/` gitlink
- `SUBMODULE.lock`

更新子模块后应同步 `SUBMODULE.lock` 中的 commit，并确保：

- `coproc-core.commit` 与 gitlink 一致。
- `protocol.transitive.commit` 和 `common.transitive.commit` 反映 coproc-core 嵌套依赖的实际 commit。

完成后执行：

```sh
git submodule update --init --recursive
git submodule status --recursive
```

未经授权不要 push、创建 PR 或改写远端历史。

## 13. 退出码

| 退出码 | 含义 |
|--------|------|
| 0 | 正常退出（收到 SIGINT/SIGTERM 或对端断开）。 |
| 2 | 命令行参数或 IPC 地址非法。 |
| 3 | IPC Hello 协商失败或对端角色不正确。 |
| 4 | OSAL / 任务 / Core 初始化失败。 |
| 5 | 运行时缓冲区分配失败。 |
