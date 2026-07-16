#include "ipc.h"

#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct writer_args {
    int fd;
    const uint8_t *data;
    size_t size;
} writer_args_t;

static void *partial_writer(void *arg) {
    writer_args_t *a = arg;
    size_t i;
    for (i = 0; i < a->size; ++i)
        if (write(a->fd, a->data + i, 1) != 1)
            return (void *)1;
    return NULL;
}

int main(void) {
    int sockets[2];
    uint8_t bytes[] = {7, 0, 0, 0, 1, 0, 0, 0, 1, 2, 3};
    uint8_t payload[16];
    size_t payload_size = 0;
    wlh_sim_record_kind_t kind;
    pthread_t thread;
    writer_args_t args;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    args.fd = sockets[0];
    args.data = bytes;
    args.size = sizeof(bytes);
    if (pthread_create(&thread, NULL, partial_writer, &args) != 0)
        return 2;
    if (wlh_sim_read_record(
            sockets[1], &kind, payload, sizeof(payload), &payload_size, 64
        ) != 0)
        return 3;
    if (kind != WLH_SIM_RECORD_WIRE_FRAME || payload_size != 3 ||
        payload[2] != 3)
        return 4;
    pthread_join(thread, NULL);
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 5;
    {
        wlh_sim_hello_t sent = {
            WLH_SIM_ROLE_COPROC, WLH_SIM_FLAG_SIDEBAND, 4096
        };
        wlh_sim_hello_t received;
        if (wlh_sim_write_hello(sockets[0], &sent) != 0 ||
            wlh_sim_read_hello(sockets[1], &received) != 0)
            return 6;
        if (received.role != WLH_SIM_ROLE_COPROC ||
            received.max_record_size != 4096)
            return 7;
    }
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 8;
    {
        const uint8_t malformed[] = {4, 0, 0, 0, 1, 0, 1, 0};
        if (write(sockets[0], malformed, sizeof(malformed)) !=
            (ssize_t)sizeof(malformed))
            return 9;
        if (wlh_sim_read_record(
                sockets[1], &kind, payload, sizeof(payload), &payload_size, 64
            ) == 0)
            return 10;
    }
    close(sockets[0]);
    close(sockets[1]);
    puts("sim IPC partial read test passed");
    return 0;
}
