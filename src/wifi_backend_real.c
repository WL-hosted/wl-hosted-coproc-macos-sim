#include "wifi_backend_real.h"
#include "wlh/log.h"

#include <stdlib.h>
#include <string.h>

/* Implemented in wifi_backend_real.swift; primitives only, no repo headers. */
typedef void (*wlh_real_bss_cb)(
    void *cb_context,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *bssid,
    uint32_t security,
    uint32_t channel,
    int32_t rssi_dbm
);

extern void *wlh_real_swift_create(void);
extern void wlh_real_swift_destroy(void *handle);
extern int32_t wlh_real_swift_initialize(void *handle);
extern int32_t wlh_real_swift_scan(
    void *handle, wlh_real_bss_cb callback, void *cb_context
);
extern int32_t wlh_real_swift_connect(
    void *handle,
    const uint8_t *ssid,
    size_t ssid_size,
    const char *password,
    wlh_real_bss_cb callback,
    void *cb_context
);
extern int32_t wlh_real_swift_disconnect(void *handle);

struct wlh_wifi_backend_real {
    wlh_coproc_t *core;
    void *swift;
};

typedef struct bss_forward {
    wlh_coproc_t *core;
    uint32_t scan_id;
} bss_forward_t;

static void forward_bss(
    void *cb_context,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *bssid,
    uint32_t security,
    uint32_t channel,
    int32_t rssi_dbm
) {
    const bss_forward_t *forward = cb_context;
    wlh_coproc_bss_t bss;
    bss.ssid = ssid;
    bss.ssid_size = ssid_size;
    memcpy(bss.bssid, bssid, sizeof(bss.bssid));
    bss.security = security;
    bss.channel = channel;
    bss.rssi_dbm = rssi_dbm;
    (void)wlh_coproc_wifi_scan_result(forward->core, forward->scan_id, &bss);
}

static void forward_connected_bss(
    void *cb_context,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *bssid,
    uint32_t security,
    uint32_t channel,
    int32_t rssi_dbm
) {
    const bss_forward_t *forward = cb_context;
    wlh_coproc_bss_t bss;
    bss.ssid = ssid;
    bss.ssid_size = ssid_size;
    memcpy(bss.bssid, bssid, sizeof(bss.bssid));
    bss.security = security;
    bss.channel = channel;
    bss.rssi_dbm = rssi_dbm;
    (void)wlh_coproc_wifi_connected(forward->core, &bss);
}

static int real_initialize(void *context) {
    wlh_wifi_backend_real_t *backend = context;
    int32_t status = wlh_real_swift_initialize(backend->swift);
    WLH_LOGI("coproc-sim", "real backend initialize status=%ld", (long)status);
    return status == 0 ? 0 : -1;
}

static int real_scan(void *context, uint32_t scan_id) {
    wlh_wifi_backend_real_t *backend = context;
    bss_forward_t forward = {backend->core, scan_id};
    int32_t count = wlh_real_swift_scan(backend->swift, forward_bss, &forward);
    WLH_LOGI("coproc-sim", "real backend scan count=%ld", (long)count);
    (void)wlh_coproc_wifi_scan_completed(
        backend->core, scan_id, count > 0 ? (uint32_t)count : 0u, false
    );
    return count < 0 ? -1 : 0;
}

static int real_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
) {
    wlh_wifi_backend_real_t *backend = context;
    char password[65];
    bss_forward_t forward = {backend->core, 0u};
    int32_t status;
    if (request->ssid_size == 0u)
        return -1;
    memcpy(password, request->credential, request->credential_size);
    password[request->credential_size] = '\0';
    WLH_LOGI(
        "coproc-sim",
        "real backend connect ssid_size=%zu security=%lu",
        request->ssid_size,
        (unsigned long)request->security
    );
    status = wlh_real_swift_connect(
        backend->swift,
        request->ssid,
        request->ssid_size,
        password,
        forward_connected_bss,
        &forward
    );
    if (status == 0)
        return 0;
    WLH_LOGW("coproc-sim", "real backend connect failed status=%ld", (long)status);
    (void)wlh_coproc_wifi_disconnected(
        backend->core, status == -2 ? 2u : 3u, false
    );
    return -1;
}

static int real_disconnect(void *context) {
    wlh_wifi_backend_real_t *backend = context;
    WLH_LOGI("coproc-sim", "real backend disconnect");
    (void)wlh_real_swift_disconnect(backend->swift);
    (void)wlh_coproc_wifi_disconnected(backend->core, 1u, true);
    return 0;
}

static int real_fault(void *context, wlh_wifi_backend_fault_t fault) {
    wlh_wifi_backend_real_t *backend = context;
    if (fault == WLH_WIFI_BACKEND_FAULT_DISCONNECT) {
        WLH_LOGI("coproc-sim", "real backend fault disconnect");
        (void)wlh_real_swift_disconnect(backend->swift);
        (void)wlh_coproc_wifi_disconnected(backend->core, 7u, false);
        return 0;
    }
    return -1;
}

wlh_wifi_backend_real_t *wlh_wifi_backend_real_create(wlh_coproc_t *core) {
    wlh_wifi_backend_real_t *backend = malloc(sizeof(*backend));
    if (backend == NULL)
        return NULL;
    backend->core = core;
    backend->swift = wlh_real_swift_create();
    if (backend->swift == NULL) {
        free(backend);
        return NULL;
    }
    return backend;
}

void wlh_wifi_backend_real_destroy(wlh_wifi_backend_real_t *backend) {
    if (backend == NULL)
        return;
    wlh_real_swift_destroy(backend->swift);
    free(backend);
}

void wlh_wifi_backend_real_fill_ops(
    wlh_wifi_backend_real_t *backend, wlh_wifi_backend_t *out_ops
) {
    out_ops->context = backend;
    out_ops->initialize = real_initialize;
    out_ops->scan = real_scan;
    out_ops->connect = real_connect;
    out_ops->disconnect = real_disconnect;
    out_ops->fault = real_fault;
}
