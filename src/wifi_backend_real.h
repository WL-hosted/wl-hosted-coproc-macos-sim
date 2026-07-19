#ifndef WLH_WIFI_BACKEND_REAL_H
#define WLH_WIFI_BACKEND_REAL_H

#include "wifi_backend.h"

/* Real macOS WiFi backend backed by CoreWLAN (implemented in Swift). */
typedef struct wlh_wifi_backend_real wlh_wifi_backend_real_t;

/* The core pointer is only dereferenced once backend ops run, so it may
 * point at storage that wlh_coproc_init fills in afterwards. */
wlh_wifi_backend_real_t *wlh_wifi_backend_real_create(wlh_coproc_t *core);
void wlh_wifi_backend_real_destroy(wlh_wifi_backend_real_t *backend);
void wlh_wifi_backend_real_fill_ops(
    wlh_wifi_backend_real_t *backend, wlh_wifi_backend_t *out_ops
);

#endif
