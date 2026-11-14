/* Send one standard ADB CNXN frame to an MHI ADB character device. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void dump_hex(const unsigned char *buf, ssize_t len)
{
	ssize_t i;
	for (i = 0; i < len; i++)
		printf("%02x%s", buf[i], (i + 1 == len) ? "\n" : " ");
}

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/mhi_ADB";
	/* command=CNXN, version=0x01000000, max payload=4096, empty banner */
	const unsigned char cnxn[] = {
		0x43, 0x4e, 0x58, 0x4e, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xbc, 0xb1, 0xa7, 0xb1
	};
	unsigned char response[4096];
	struct pollfd pfd;
	ssize_t len;
	int fd;

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror(device);
		return 2;
	}
	if (write(fd, cnxn, sizeof(cnxn)) != (ssize_t)sizeof(cnxn)) {
		perror("write CNXN");
		close(fd);
		return 3;
	}
	printf("CNXN sent; waiting up to 10 seconds for the modem reply...\n");
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 10000) <= 0) {
		printf("No ADB response.\n");
		close(fd);
		return 4;
	}
	len = read(fd, response, sizeof(response));
	if (len <= 0) {
		perror("read reply");
		close(fd);
		return 5;
	}
	printf("Received %zd byte(s): ", len);
	dump_hex(response, len);
	if (len >= 4 && !memcmp(response, "CNXN", 4))
		printf("Valid ADB CNXN reply received.\n");
	close(fd);
	return 0;
}
