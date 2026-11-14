/* Read-only ADB service probe for /dev/mhi_ADB: CNXN then OPEN shell:id. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct adb_header {
	uint32_t command, arg0, arg1, length, checksum, magic;
};

static uint32_t checksum(const unsigned char *buf, size_t len)
{
	uint32_t sum = 0;
	while (len--)
		sum += *buf++;
	return sum;
}

static int wait_read(int fd, unsigned char *buf, size_t need, int timeout_ms)
{
	size_t got = 0;
	while (got < need) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int ready = poll(&pfd, 1, timeout_ms);
		ssize_t n;
		if (ready <= 0)
			return -1;
		n = read(fd, buf + got, need - got);
		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == 512))
			continue;
		return -1;
	}
	return 0;
}

static int send_packet(int fd, const char command[4], uint32_t arg0,
			       uint32_t arg1, const unsigned char *data, uint32_t length)
{
	struct adb_header header = { 0 };
	header.command = ((uint32_t)(unsigned char)command[0]) |
		((uint32_t)(unsigned char)command[1] << 8) |
		((uint32_t)(unsigned char)command[2] << 16) |
		((uint32_t)(unsigned char)command[3] << 24);
	header.arg0 = arg0;
	header.arg1 = arg1;
	header.length = length;
	header.checksum = checksum(data, length);
	header.magic = header.command ^ 0xffffffffU;
	if (write(fd, &header, sizeof(header)) != (ssize_t)sizeof(header))
		return -1;
	if (length && write(fd, data, length) != (ssize_t)length)
		return -1;
	return 0;
}

static int receive_packet(int fd, struct adb_header *header,
			  unsigned char **data)
{
	*data = NULL;
	if (wait_read(fd, (unsigned char *)header, sizeof(*header), 5000))
		return -1;
	if (header->length > 16384)
		return -1;
	if (header->length) {
		*data = calloc(1, header->length + 1);
		if (!*data || wait_read(fd, *data, header->length, 5000)) {
			free(*data);
			return -1;
		}
	}
	return 0;
}

static void show_packet(const struct adb_header *h, const unsigned char *data)
{
	char command[5];
	memcpy(command, &h->command, 4);
	command[4] = '\0';
	printf("%s arg0=%u arg1=%u length=%u", command, h->arg0, h->arg1, h->length);
	if (data && h->length)
		printf(" data=%.*s", (int)h->length, data);
	putchar('\n');
}

int main(void)
{
	const unsigned char shell[] = "shell:id";
	struct adb_header h;
	unsigned char *data;
	int fd = open("/dev/mhi_ADB", O_RDWR);
	int i;

	if (fd < 0) {
		perror("/dev/mhi_ADB");
		return 2;
	}
	if (send_packet(fd, "CNXN", 0x01000000, 4096, NULL, 0) ||
	    receive_packet(fd, &h, &data)) {
		fprintf(stderr, "No valid CNXN reply\n");
		close(fd);
		return 3;
	}
	show_packet(&h, data);
	free(data);
	if (memcmp(&h.command, "CNXN", 4)) {
		close(fd);
		return 4;
	}
	if (send_packet(fd, "OPEN", 1, 0, shell, sizeof(shell))) {
		fprintf(stderr, "Could not send shell OPEN\n");
		close(fd);
		return 5;
	}
	printf("Opened shell:id; modem replies:\n");
	for (i = 0; i < 4; i++) {
		if (receive_packet(fd, &h, &data))
			break;
		show_packet(&h, data);
		free(data);
		if (!memcmp(&h.command, "CLSE", 4))
			break;
	}
	close(fd);
	return 0;
}
