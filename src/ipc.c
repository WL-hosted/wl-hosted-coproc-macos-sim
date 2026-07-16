#include "ipc.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static const uint8_t hello_magic[8] = {'W', 'L', 'H', 'S', 'I', 'M', 0, 0};

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size != 0u) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, uint8_t *data, size_t size)
{
    while (size != 0u) {
        ssize_t received = read(fd, data, size);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return -1;
        data += (size_t)received;
        size -= (size_t)received;
    }
    return 0;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int wlh_sim_write_hello(int fd, const wlh_sim_hello_t *hello)
{
    uint8_t bytes[WLH_SIM_HELLO_SIZE] = {0};
    if (hello == NULL || hello->role < WLH_SIM_ROLE_HOST || hello->role > WLH_SIM_ROLE_MANAGER ||
        (hello->flags & (uint8_t)~WLH_SIM_FLAG_SIDEBAND) != 0u ||
        hello->max_record_size < 8u || hello->max_record_size > WLH_SIM_MAX_RECORD_SIZE) return -1;
    memcpy(bytes, hello_magic, sizeof(hello_magic));
    bytes[8] = 1u;
    bytes[10] = (uint8_t)hello->role;
    bytes[11] = hello->flags;
    write_u32(bytes + 12, hello->max_record_size);
    return write_all(fd, bytes, sizeof(bytes));
}

int wlh_sim_read_hello(int fd, wlh_sim_hello_t *hello)
{
    uint8_t bytes[WLH_SIM_HELLO_SIZE];
    if (hello == NULL || read_all(fd, bytes, sizeof(bytes)) != 0 ||
        memcmp(bytes, hello_magic, sizeof(hello_magic)) != 0 || bytes[8] != 1u || bytes[9] != 0u ||
        bytes[10] < WLH_SIM_ROLE_HOST || bytes[10] > WLH_SIM_ROLE_MANAGER ||
        (bytes[11] & (uint8_t)~WLH_SIM_FLAG_SIDEBAND) != 0u) return -1;
    hello->role = (wlh_sim_role_t)bytes[10];
    hello->flags = bytes[11];
    hello->max_record_size = read_u32(bytes + 12);
    return hello->max_record_size >= 8u && hello->max_record_size <= WLH_SIM_MAX_RECORD_SIZE ? 0 : -1;
}

int wlh_sim_write_record(
    int fd, wlh_sim_record_kind_t kind, const uint8_t *payload, size_t payload_size,
    uint32_t max_record_size)
{
    uint8_t header[8] = {0};
    if (kind < WLH_SIM_RECORD_WIRE_FRAME || kind > WLH_SIM_RECORD_FAULT_RESPONSE ||
        (payload == NULL && payload_size != 0u) || max_record_size < 8u ||
        payload_size > (size_t)UINT32_MAX - 4u ||
        payload_size > (size_t)max_record_size - 8u) return -1;
    write_u32(header, (uint32_t)payload_size + 4u);
    header[4] = (uint8_t)kind;
    return write_all(fd, header, sizeof(header)) == 0 && write_all(fd, payload, payload_size) == 0 ? 0 : -1;
}

int wlh_sim_read_record(
    int fd, wlh_sim_record_kind_t *kind, uint8_t *payload, size_t capacity,
    size_t *payload_size, uint32_t max_record_size)
{
    uint8_t header[8];
    uint32_t record_len;
    if (kind == NULL || payload == NULL || payload_size == NULL || max_record_size < 8u ||
        read_all(fd, header, sizeof(header)) != 0) return -1;
    record_len = read_u32(header);
    if (record_len < 4u || record_len > max_record_size - 4u || record_len - 4u > capacity ||
        header[4] < WLH_SIM_RECORD_WIRE_FRAME || header[4] > WLH_SIM_RECORD_FAULT_RESPONSE ||
        header[5] != 0u || header[6] != 0u || header[7] != 0u) return -1;
    *kind = (wlh_sim_record_kind_t)header[4];
    *payload_size = record_len - 4u;
    return read_all(fd, payload, *payload_size);
}
