#include "ipc.h"
#include "posix_osal.h"
#include "wlh/coproc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "sim_sideband.pb.h"
#include "wifi.pb.h"

typedef enum scenario {
    SCENARIO_HAPPY,
    SCENARIO_AUTH_FAIL,
    SCENARIO_AP_NOT_FOUND
} scenario_t;

typedef struct simulator {
    int fd;
    uint32_t max_record_size;
    bool sideband;
    scenario_t scenario;
    uint32_t monitor_interval_ms;
    uint64_t next_monitor_ms;
    bool scan_failure;
    atomic_uint buffer_oom_remaining;
    wlh_posix_osal_t posix_osal;
    wlh_osal_ops_t osal;
    wlh_osal_task_t tx_task;
    wlh_osal_queue_t tx_queue;
    void *tx_queue_storage[16];
    wlh_osal_task_t wifi_task;
    wlh_osal_queue_t wifi_queue;
    void *wifi_queue_storage[16];
    uint32_t backend_delay_ms;
    wlh_coproc_t core;
} simulator_t;

typedef struct tx_work {
    uint8_t *frame;
    size_t size;
    wlh_coproc_tx_complete_fn completion;
    void *completion_context;
} tx_work_t;

typedef enum wifi_job_kind {
    WIFI_JOB_STOP = 0,
    WIFI_JOB_INITIALIZE,
    WIFI_JOB_SCAN,
    WIFI_JOB_CONNECT,
    WIFI_JOB_DISCONNECT
} wifi_job_kind_t;

typedef struct wifi_job {
    wifi_job_kind_t kind;
    uint32_t operation_id;
    uint32_t scan_id;
    wlh_coproc_wifi_connect_t connect;
} wifi_job_t;

static volatile sig_atomic_t running = 1;
static const uint8_t ssid_open[] = "OpenLab";
static const uint8_t ssid_wpa2[] = "WPA2Net";
static const uint8_t ssid_wpa3[] = "WPA3Net";
// clang-format off
static const wlh_coproc_bss_t networks[3] = {
    {ssid_open, 7, {0x02, 0, 0, 0, 0, 1}, 1, 1, -35},
    {ssid_wpa2, 7, {0x02, 0, 0, 0, 0, 2}, 4, 6, -48},
    {ssid_wpa3, 7, {0x02, 0, 0, 0, 0, 3}, 6, 36, -57},
};
// clang-format on

static uint64_t monotonic_ms(void *context) {
    struct timespec value;
    (void)context;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static uint8_t *buffer_alloc(void *context, size_t size) {
    simulator_t *sim = context;
    unsigned remaining = atomic_load(&sim->buffer_oom_remaining);
    while (remaining != 0u) {
        if (atomic_compare_exchange_weak(
                &sim->buffer_oom_remaining, &remaining, remaining - 1u
            ))
            return NULL;
    }
    return malloc(size);
}

static void buffer_free(void *context, uint8_t *buffer) {
    (void)context;
    free(buffer);
}

static void tx_worker(void *argument) {
    simulator_t *sim = argument;
    for (;;) {
        tx_work_t *work = NULL;
        if (sim->osal.queue_receive(
                sim->osal.context, &sim->tx_queue, &work,
                WLH_OSAL_WAIT_FOREVER
            ) != 0)
            continue;
        if (work == NULL)
            break;
        {
            int status = wlh_sim_write_record(
                sim->fd, WLH_SIM_RECORD_WIRE_FRAME, work->frame, work->size,
                sim->max_record_size
            );
            work->completion(
                work->completion_context, work->frame, work->size, status
            );
        }
        free(work);
    }
}

static int transport_submit(
    void *context, uint8_t *frame, size_t size,
    wlh_coproc_tx_complete_fn completion, void *completion_context
) {
    simulator_t *sim = context;
    tx_work_t *work = malloc(sizeof(*work));
    if (work == NULL)
        return -1;
    *work = (tx_work_t){frame, size, completion, completion_context};
    if (sim->osal.queue_send(
            sim->osal.context, &sim->tx_queue, &work, WLH_OSAL_NO_WAIT
        ) != 0) {
        free(work);
        return -1;
    }
    return 0;
}

static void ethernet_echo(void *context, const uint8_t *frame, size_t size) {
    simulator_t *sim = context;
    (void)wlh_coproc_ethernet_sta_send(&sim->core, frame, size);
}

static int wifi_do_initialize(void *context) {
    simulator_t *sim = context;
    return atomic_load(&sim->buffer_oom_remaining) != 0u ? -1 : 0;
}

static int wifi_do_scan(void *context, uint32_t scan_id) {
    simulator_t *sim = context;
    size_t i;
    if (sim->scan_failure) {
        (void)wlh_coproc_wifi_scan_completed(&sim->core, scan_id, 0, false);
        return -1;
    }
    for (i = 0; i < 3u; ++i) {
        (void)wlh_coproc_wifi_scan_result(&sim->core, scan_id, &networks[i]);
        sim->osal.sleep_ms(sim->osal.context, 10u);
    }
    (void)wlh_coproc_wifi_scan_completed(&sim->core, scan_id, 3, false);
    return 0;
}

static bool bytes_equal(
    const uint8_t *left, size_t left_size, const char *right
) {
    size_t right_size = strlen(right);
    return left_size == right_size && memcmp(left, right, right_size) == 0;
}

static int wifi_do_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
) {
    simulator_t *sim = context;
    size_t i;
    const wlh_coproc_bss_t *match = NULL;
    if (sim->scenario == SCENARIO_AP_NOT_FOUND) {
        (void)wlh_coproc_wifi_disconnected(&sim->core, 2u, false);
        return -1;
    }
    for (i = 0; i < 3u; ++i) {
        if (request->ssid_size == networks[i].ssid_size &&
            memcmp(request->ssid, networks[i].ssid, request->ssid_size) == 0)
            match = &networks[i];
    }
    if (match == NULL) {
        (void)wlh_coproc_wifi_disconnected(&sim->core, 2u, false);
        return -1;
    }
    // clang-format off
    if (sim->scenario == SCENARIO_AUTH_FAIL ||
        request->security != match->security ||
        (match->security == 4u &&
         !bytes_equal(request->credential, request->credential_size,
                      "password123")) ||
        (match->security == 6u &&
         !bytes_equal(request->credential, request->credential_size,
                      "sae-secret"))) {
        (void)wlh_coproc_wifi_disconnected(&sim->core, 3u, false);
        return -1;
    }
    // clang-format on
    (void)wlh_coproc_wifi_connected(&sim->core, match);
    return 0;
}

static int wifi_do_disconnect(void *context) {
    simulator_t *sim = context;
    (void)wlh_coproc_wifi_disconnected(&sim->core, 1u, true);
    return 0;
}

static int wifi_submit_job(simulator_t *sim, const wifi_job_t *job) {
    wifi_job_t *copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return -1;
    *copy = *job;
    if (sim->osal.queue_send(
            sim->osal.context, &sim->wifi_queue, &copy, WLH_OSAL_NO_WAIT
        ) != 0) {
        free(copy);
        return -1;
    }
    return 0;
}

static int wifi_initialize(void *context, uint32_t operation_id) {
    wifi_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = WIFI_JOB_INITIALIZE;
    job.operation_id = operation_id;
    return wifi_submit_job(context, &job);
}

static int wifi_scan(void *context, uint32_t scan_id) {
    wifi_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = WIFI_JOB_SCAN;
    job.scan_id = scan_id;
    return wifi_submit_job(context, &job);
}

static int wifi_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
) {
    wifi_job_t job;
    if (request == NULL)
        return -1;
    memset(&job, 0, sizeof(job));
    job.kind = WIFI_JOB_CONNECT;
    job.connect = *request;
    return wifi_submit_job(context, &job);
}

static int wifi_disconnect(void *context) {
    wifi_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = WIFI_JOB_DISCONNECT;
    return wifi_submit_job(context, &job);
}

static void wifi_worker(void *argument) {
    simulator_t *sim = argument;
    for (;;) {
        wifi_job_t *job = NULL;
        if (sim->osal.queue_receive(
                sim->osal.context, &sim->wifi_queue, &job,
                WLH_OSAL_WAIT_FOREVER
            ) != 0)
            continue;
        if (job == NULL || job->kind == WIFI_JOB_STOP) {
            free(job);
            break;
        }
        sim->osal.sleep_ms(sim->osal.context, sim->backend_delay_ms);
        if (job->kind == WIFI_JOB_INITIALIZE) {
            int status = wifi_do_initialize(sim);
            (void)wlh_coproc_wifi_initialized(
                &sim->core, job->operation_id, status
            );
        }
        else if (job->kind == WIFI_JOB_SCAN)
            (void)wifi_do_scan(sim, job->scan_id);
        else if (job->kind == WIFI_JOB_CONNECT)
            (void)wifi_do_connect(sim, &job->connect);
        else if (job->kind == WIFI_JOB_DISCONNECT)
            (void)wifi_do_disconnect(sim);
        free(job);
    }
}

static int open_unix(const char *spec) {
    struct sockaddr_un address;
    int fd;
    if (strncmp(spec, "fd:", 3) == 0)
        return atoi(spec + 3);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strncmp(spec, "connect:", 8) == 0) {
        if (strlen(spec + 8) >= sizeof(address.sun_path))
            return -1;
        memcpy(address.sun_path, spec + 8, strlen(spec + 8) + 1u);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 ||
            connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
            return -1;
        return fd;
    }

    if (strncmp(spec, "listen:", 7) == 0) {
        int peer;
        if (strlen(spec + 7) >= sizeof(address.sun_path))
            return -1;
        memcpy(address.sun_path, spec + 7, strlen(spec + 7) + 1u);
        unlink(address.sun_path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 ||
            bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(fd, 1) != 0)
            return -1;
        peer = accept(fd, NULL, NULL);
        close(fd);
        return peer;
    }
    return -1;
}

static int send_runtime(simulator_t *sim) {
    uint8_t payload[1024];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    wlh_coproc_diagnostics_t diagnostics;
    wlh_sim_v1_SimRuntimeInfo info = wlh_sim_v1_SimRuntimeInfo_init_zero;
    wlh_coproc_get_diagnostics(&sim->core, &diagnostics);
    info.role = wlh_sim_v1_SimRole_SIM_ROLE_COPROCESSOR;
    info.link_state = diagnostics.state == WLH_COPROC_STATE_READY
                          ? wlh_sim_v1_SimLinkState_SIM_LINK_STATE_UP
                          : wlh_sim_v1_SimLinkState_SIM_LINK_STATE_DOWN;
    info.session_id = diagnostics.session_id;
    info.uptime_ms = monotonic_ms(NULL);
    info.tx_frames = diagnostics.tx_frames;
    info.rx_frames = diagnostics.rx_frames;
    info.free_buffers =
        atomic_load(&sim->buffer_oom_remaining) == 0u ? 64u : 0u;
    memcpy(
        info.implementation,
        "wlh-coproc-macos-sim",
        sizeof("wlh-coproc-macos-sim")
    );
    memcpy(info.implementation_version, "0.1.0", sizeof("0.1.0"));
    if (!pb_encode(&stream, wlh_sim_v1_SimRuntimeInfo_fields, &info))
        return -1;
    return wlh_sim_write_record(
        sim->fd,
        WLH_SIM_RECORD_RUNTIME_INFO,
        payload,
        stream.bytes_written,
        sim->max_record_size
    );
}

static int handle_fault(simulator_t *sim, const uint8_t *payload, size_t size) {
    uint8_t response_data[256];
    pb_istream_t input = pb_istream_from_buffer(payload, size);
    pb_ostream_t output =
        pb_ostream_from_buffer(response_data, sizeof(response_data));
    wlh_sim_v1_SimFaultRequest request = wlh_sim_v1_SimFaultRequest_init_zero;
    wlh_sim_v1_SimFaultResponse response =
        wlh_sim_v1_SimFaultResponse_init_zero;
    bool accepted = true;
    if (!pb_decode(&input, wlh_sim_v1_SimFaultRequest_fields, &request) ||
        request.request_id == 0u || request.channel > 255u ||
        request.count > 1024u || request.duration_ms > 60000u ||
        request.parameters.size > 256u)
        return -1;
    switch (request.fault) {
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_SESSION_CHANGE:
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_PEER_RESET:
        wlh_coproc_test_reset_session(&sim->core, 1u);
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_CLEAR_CREDIT:
        wlh_coproc_test_set_credit(&sim->core, 1u, 0u);
        wlh_coproc_test_set_credit(&sim->core, 2u, 0u);
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_CHANNEL_RESET:
        wlh_coproc_test_reset_channel(&sim->core, (uint8_t)request.channel);
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_BUFFER_OOM:
        atomic_store(
            &sim->buffer_oom_remaining,
            request.count != 0u ? request.count : 1u
        );
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_LIMIT_CREDIT:
        wlh_coproc_test_set_credit(
            &sim->core, (uint8_t)request.channel, request.count
        );
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_WIFI_DISCONNECT:
        (void)wlh_coproc_wifi_disconnected(&sim->core, 7u, false);
        break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_WIFI_SCAN_FAILURE:
        sim->scan_failure = true;
        break;
    default:
        accepted = false;
        break;
    }
    response.request_id = request.request_id;
    response.accepted = accepted;
    response.status_code = accepted ? 0 : -2;
    memcpy(
        response.detail,
        accepted ? "accepted" : "not supported",
        accepted ? sizeof("accepted") : sizeof("not supported")
    );
    if (!pb_encode(&output, wlh_sim_v1_SimFaultResponse_fields, &response))
        return -1;
    return wlh_sim_write_record(
        sim->fd,
        WLH_SIM_RECORD_FAULT_RESPONSE,
        response_data,
        output.bytes_written,
        sim->max_record_size
    );
}

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

int main(int argc, char **argv) {
    const char *ipc_spec = NULL;
    char manager_ipc[sizeof(((struct sockaddr_un *)0)->sun_path) + 9u];
    simulator_t sim;
    wlh_coproc_config_t config;
    wlh_osal_task_attributes_t tx_attributes = {
        "wlh-coproc-posix-tx", 0u, 0
    };
    wlh_osal_task_attributes_t wifi_attributes = {
        "wlh-coproc-mock-wifi", 0u, 0
    };
    // clang-format off
    wlh_sim_hello_t local = {
        WLH_SIM_ROLE_COPROC,
        WLH_SIM_FLAG_SIDEBAND,
        WLH_SIM_MAX_RECORD_SIZE,
    };
    // clang-format on
    wlh_sim_hello_t peer;
    uint8_t *record;
    int i;
    memset(&sim, 0, sizeof(sim));
    atomic_init(&sim.buffer_oom_remaining, 0u);
    sim.backend_delay_ms = 25u;
    sim.monitor_interval_ms = 1000u;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ipc") == 0 && i + 1 < argc)
            ipc_spec = argv[++i];
        else if (strcmp(argv[i], "--manager-socket") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            if (strlen(path) + sizeof("connect:") > sizeof(manager_ipc))
                return 2;
            memcpy(manager_ipc, "connect:", sizeof("connect:") - 1u);
            memcpy(
                manager_ipc + sizeof("connect:") - 1u, path, strlen(path) + 1u
            );
            ipc_spec = manager_ipc;
        } else if (strcmp(argv[i], "--monitor-interval-ms") == 0 &&
                   i + 1 < argc)
            sim.monitor_interval_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            const char *scenario = argv[++i];
            if (strcmp(scenario, "auth-fail") == 0)
                sim.scenario = SCENARIO_AUTH_FAIL;
            else if (strcmp(scenario, "ap-not-found") == 0)
                sim.scenario = SCENARIO_AP_NOT_FOUND;
            else if (strcmp(scenario, "happy") != 0)
                return 2;
        } else {
            fprintf(stderr, "invalid argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (ipc_spec == NULL || sim.monitor_interval_ms == 0u ||
        sim.monitor_interval_ms > 60000u || (sim.fd = open_unix(ipc_spec)) < 0)
        return 2;

    if (wlh_sim_write_hello(sim.fd, &local) != 0 ||
        wlh_sim_read_hello(sim.fd, &peer) != 0)
        return 3;
    if (peer.role != WLH_SIM_ROLE_HOST && peer.role != WLH_SIM_ROLE_MANAGER)
        return 3;
    sim.max_record_size = peer.max_record_size < local.max_record_size
                              ? peer.max_record_size
                              : local.max_record_size;
    sim.sideband = peer.role == WLH_SIM_ROLE_MANAGER &&
                   (peer.flags & local.flags & WLH_SIM_FLAG_SIDEBAND) != 0u;

    wlh_posix_osal_init(&sim.posix_osal);
    sim.osal = wlh_posix_osal_ops(&sim.posix_osal);
    if (sim.osal.queue_create(
            sim.osal.context, &sim.tx_queue, sim.tx_queue_storage,
            sizeof(void *), 16u
        ) != 0 ||
        sim.osal.task_create(
            sim.osal.context, &sim.tx_task, &tx_attributes, tx_worker, &sim
        ) != 0 ||
        sim.osal.queue_create(
            sim.osal.context, &sim.wifi_queue, sim.wifi_queue_storage,
            sizeof(void *), 16u
        ) != 0 ||
        sim.osal.task_create(
            sim.osal.context, &sim.wifi_task, &wifi_attributes,
            wifi_worker, &sim
        ) != 0)
        return 4;

    memset(&config, 0, sizeof(config));
    config.port.context = &sim;
    config.port.submit_tx = transport_submit;
    config.port.ethernet_rx = ethernet_echo;
    config.buffers = (wlh_coproc_buffer_ops_t){
        &sim, buffer_alloc, buffer_free
    };
    config.osal = sim.osal;

    config.wifi.context = &sim;
    config.wifi.initialize = wifi_initialize;
    config.wifi.scan = wifi_scan;
    config.wifi.connect = wifi_connect;
    config.wifi.disconnect = wifi_disconnect;

    config.max_frame_size = 4096u;
    config.heartbeat_interval_ms = 1000u;
    config.initial_credit = 64u;
    config.initial_session_id = 1u;
    config.core_queue_depth = 16u;
    config.stop_timeout_ms = 3000u;

    if (wlh_coproc_init(&sim.core, &config) != WLH_COPROC_OK ||
        wlh_coproc_start(&sim.core) != WLH_COPROC_OK)
        return 4;
    record = malloc(sim.max_record_size);
    if (record == NULL)
        return 5;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sim.next_monitor_ms = monotonic_ms(NULL) + sim.monitor_interval_ms;
    while (running) {
        struct pollfd poll_fd = {sim.fd, POLLIN, 0};
        int ready = poll(&poll_fd, 1, 50);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready > 0 &&
            (poll_fd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            break;
        if (ready > 0 && (poll_fd.revents & POLLIN) != 0) {
            wlh_sim_record_kind_t kind;
            size_t size;
            if (wlh_sim_read_record(
                    sim.fd,
                    &kind,
                    record,
                    sim.max_record_size,
                    &size,
                    sim.max_record_size
                ) != 0)
                break;
            if (kind == WLH_SIM_RECORD_WIRE_FRAME) {
                (void)wlh_coproc_on_frame(&sim.core, record, size);
            } else if (!sim.sideband) {
                break;
            } else if (kind == WLH_SIM_RECORD_FAULT_REQUEST) {
                (void)handle_fault(&sim, record, size);
            } else {
                break;
            }
        }
        if (sim.sideband && monotonic_ms(NULL) >= sim.next_monitor_ms) {
            (void)send_runtime(&sim);
            sim.next_monitor_ms = monotonic_ms(NULL) + sim.monitor_interval_ms;
        }
    }

    free(record);
    (void)wlh_coproc_stop(&sim.core);
    {
        wifi_job_t *stop = NULL;
        (void)sim.osal.queue_send(
            sim.osal.context, &sim.wifi_queue, &stop,
            WLH_OSAL_WAIT_FOREVER
        );
        (void)sim.osal.task_join(
            sim.osal.context, &sim.wifi_task, 3000u
        );
        sim.osal.queue_destroy(sim.osal.context, &sim.wifi_queue);
    }
    {
        tx_work_t *stop = NULL;
        (void)sim.osal.queue_send(
            sim.osal.context, &sim.tx_queue, &stop, WLH_OSAL_WAIT_FOREVER
        );
        (void)sim.osal.task_join(
            sim.osal.context, &sim.tx_task, 3000u
        );
        sim.osal.queue_destroy(sim.osal.context, &sim.tx_queue);
    }
    close(sim.fd);
    return 0;
}
