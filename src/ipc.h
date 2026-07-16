#ifndef WLH_SIM_IPC_H
#define WLH_SIM_IPC_H

#include <stddef.h>
#include <stdint.h>

#define WLH_SIM_HELLO_SIZE 16u
#define WLH_SIM_MAX_RECORD_SIZE 1048584u
#define WLH_SIM_FLAG_SIDEBAND 1u

typedef enum wlh_sim_role {
    WLH_SIM_ROLE_HOST = 1,
    WLH_SIM_ROLE_COPROC = 2,
    WLH_SIM_ROLE_MANAGER = 3
} wlh_sim_role_t;

typedef enum wlh_sim_record_kind {
    WLH_SIM_RECORD_WIRE_FRAME = 1,
    WLH_SIM_RECORD_RUNTIME_INFO = 2,
    WLH_SIM_RECORD_FAULT_REQUEST = 3,
    WLH_SIM_RECORD_FAULT_RESPONSE = 4
} wlh_sim_record_kind_t;

typedef struct wlh_sim_hello {
    wlh_sim_role_t role;
    uint8_t flags;
    uint32_t max_record_size;
} wlh_sim_hello_t;

int wlh_sim_write_hello(int fd, const wlh_sim_hello_t *hello);
int wlh_sim_read_hello(int fd, wlh_sim_hello_t *hello);

int wlh_sim_write_record(
    int fd,
    wlh_sim_record_kind_t kind,
    const uint8_t *payload,
    size_t payload_size,
    uint32_t max_record_size
);
int wlh_sim_read_record(
    int fd,
    wlh_sim_record_kind_t *kind,
    uint8_t *payload,
    size_t capacity,
    size_t *payload_size,
    uint32_t max_record_size
);

#endif
