/*
 * One-shot ADB shell command over Quectel's PCIe MHI ADB channel.
 *
 * This bypasses TCP adb and the bridge's reconnect quirks.  It is deliberately
 * non-interactive: one command, collect WRTE output, acknowledge each packet,
 * then close.  Intended for controlled maintenance on /dev/mhi_ADB.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CMD(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define CNXN CMD('C','N','X','N')
#define OPEN CMD('O','P','E','N')
#define OKAY CMD('O','K','A','Y')
#define WRTE CMD('W','R','T','E')
#define CLSE CMD('C','L','S','E')

struct adb_header { uint32_t command, arg0, arg1, length, checksum, magic; };

static uint32_t sum(const unsigned char *p, size_t n) { uint32_t s = 0; while (n--) s += *p++; return s; }

static int read_all(int fd, void *buf, size_t length, int timeout_ms) {
    unsigned char *p = buf;
    while (length) {
        struct pollfd fds = { .fd = fd, .events = POLLIN };
        ssize_t n;
        if (poll(&fds, 1, timeout_ms) <= 0) return -1;
        n = read(fd, p, length);
        if (n > 0) { p += n; length -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == 512)) continue;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t length) {
    const unsigned char *p = buf;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n > 0) { p += n; length -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int send_packet(int fd, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, uint32_t length) {
    struct adb_header h = { cmd, arg0, arg1, length, sum(data, length), cmd ^ 0xffffffffU };
    return write_all(fd, &h, sizeof(h)) || (length && write_all(fd, data, length));
}

static int recv_packet(int fd, struct adb_header *h, unsigned char **data) {
    *data = NULL;
    if (read_all(fd, h, sizeof(*h), 7000) || h->magic != (h->command ^ 0xffffffffU) || h->length > 65536)
        return -1;
    if (h->length) {
        *data = malloc(h->length);
        if (!*data || read_all(fd, *data, h->length, 7000)) { free(*data); *data = NULL; return -1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *device = "/dev/mhi_ADB";
    char *service;
    size_t used = 0, size = 1;
    int fd, opened = 0;
    uint32_t remote_id = 0;

    if (argc < 2) { fprintf(stderr, "Usage: %s <shell command> [argument ...]\n", argv[0]); return 2; }
    for (int i = 1; i < argc; i++) size += strlen(argv[i]) + (i + 1 < argc ? 1 : 0);
    service = malloc(7 + size);
    if (!service) return 2;
    memcpy(service, "shell:", 6); used = 6;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]); memcpy(service + used, argv[i], n); used += n;
        if (i + 1 < argc) service[used++] = ' ';
    }
    service[used++] = '\0';

    fd = open(device, O_RDWR);
    if (fd < 0) { perror(device); free(service); return 3; }
    if (send_packet(fd, CNXN, 0x01000000, 4096, NULL, 0)) goto failed;

    for (;;) {
        struct adb_header h; unsigned char *data;
        if (recv_packet(fd, &h, &data)) goto failed;
        free(data);
        if (h.command == CNXN) break;
    }
    if (send_packet(fd, OPEN, 1, 0, service, (uint32_t)used)) goto failed;

    for (;;) {
        struct adb_header h; unsigned char *data;
        if (recv_packet(fd, &h, &data)) goto failed;
        if (h.command == CNXN) { free(data); continue; }
        if (h.command == OKAY && h.arg1 == 1) { remote_id = h.arg0; opened = 1; free(data); continue; }
        if (h.command == WRTE && h.arg1 == 1) {
            if (h.length) fwrite(data, 1, h.length, stdout);
            fflush(stdout);
            if (send_packet(fd, OKAY, 1, h.arg0, NULL, 0)) { free(data); goto failed; }
            free(data);
            continue;
        }
        if (h.command == CLSE && h.arg1 == 1) {
            free(data);
            if (opened) send_packet(fd, CLSE, 1, h.arg0 ? h.arg0 : remote_id, NULL, 0);
            close(fd); free(service); return 0;
        }
        free(data);
    }

failed:
    fprintf(stderr, "MHI ADB command did not complete\n");
    close(fd); free(service); return 4;
}
