#ifndef WLH_WIFI_BACKEND_H
#define WLH_WIFI_BACKEND_H

#include "wlh/coproc.h"

#include <stdint.h>

/* Backend-specific fault kinds, mapped from sideband fault requests by main.c.
 */
typedef enum wlh_wifi_backend_fault {
    WLH_WIFI_BACKEND_FAULT_SCAN_FAILURE = 0,
    WLH_WIFI_BACKEND_FAULT_DISCONNECT
} wlh_wifi_backend_fault_t;

/*
 * WiFi backend vtable. All functions run on the wifi worker task and may
 * block; results are reported through the wlh_coproc_wifi_* ingress APIs.
 */
typedef struct wlh_wifi_backend {
    void *context;
    /* Returns the status passed to wlh_coproc_wifi_initialized. */
    int (*initialize)(void *context, uint32_t interface_flags);
    int (*scan)(void *context, uint32_t scan_id);
    int (*connect)(void *context, const wlh_coproc_wifi_connect_t *request);
    int (*disconnect)(void *context);
    /* Returns 0 when the fault was handled, nonzero when unsupported. */
    int (*fault)(void *context, wlh_wifi_backend_fault_t fault);
} wlh_wifi_backend_t;

#endif
