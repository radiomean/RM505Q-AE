/*
 * mhi-qmanager-lan-proxy
 *
 * Exposes exactly one modem-local TCP port over the already-local
 * mhi-adb-bridge transport.  It is deliberately not a general purpose ADB
 * proxy.  The intended invocation is:
 *
 *   mhi-qmanager-lan-proxy 192.168.8.1 8443 443
 *
 * which binds QManager HTTPS only to the OpenWrt LAN address.  The raw MHI
 * ADB bridge remains loopback-only on 127.0.0.1:6555.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ADB_HEADER_SIZE 24
#define MAX_PAYLOAD 4096
#define CLIENT_BUFFER 16384

#define CMD(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
	((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define CNXN CMD('C','N','X','N')
#define OPEN CMD('O','P','E','N')
#define OKAY CMD('O','K','A','Y')
#define CLSE CMD('C','L','S','E')
#define WRTE CMD('W','R','T','E')

struct adb_packet {
	uint32_t command;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t length;
	uint32_t check;
	uint32_t magic;
	unsigned char payload[MAX_PAYLOAD];
};

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static uint32_t checksum(const unsigned char *p, size_t len)
{
	uint32_t total = 0;
	while (len--)
		total += *p++;
	return total;
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
	while (len) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
		if (n > 0) {
			buf += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int read_exact(int fd, unsigned char *buf, size_t len)
{
	while (len) {
		ssize_t n = recv(fd, buf, len, 0);
		if (n > 0) {
			buf += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int send_packet(int fd, uint32_t command, uint32_t arg0,
	uint32_t arg1, const unsigned char *payload, uint32_t length)
{
	unsigned char header[ADB_HEADER_SIZE] = { 0 };
	put_le32(header, command);
	put_le32(header + 4, arg0);
	put_le32(header + 8, arg1);
	put_le32(header + 12, length);
	put_le32(header + 16, checksum(payload, length));
	put_le32(header + 20, command ^ 0xffffffffU);
	if (write_all(fd, header, sizeof(header)) < 0)
		return -1;
	return length ? write_all(fd, payload, length) : 0;
}

static int receive_packet(int fd, struct adb_packet *packet)
{
	unsigned char header[ADB_HEADER_SIZE];
	if (read_exact(fd, header, sizeof(header)) < 0)
		return -1;
	packet->command = le32(header);
	packet->arg0 = le32(header + 4);
	packet->arg1 = le32(header + 8);
	packet->length = le32(header + 12);
	packet->check = le32(header + 16);
	packet->magic = le32(header + 20);
	if (packet->magic != (packet->command ^ 0xffffffffU) ||
		packet->length > MAX_PAYLOAD)
		return -1;
	if (packet->length && read_exact(fd, packet->payload, packet->length) < 0)
		return -1;
	return checksum(packet->payload, packet->length) == packet->check ? 0 : -1;
}

static int connect_bridge(void)
{
	struct sockaddr_in address;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(6555);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int wait_for_cnxn(int adb)
{
	struct adb_packet packet;
	if (send_packet(adb, CNXN, 0x01000000U, 0x1000U, NULL, 0) < 0)
		return -1;
	for (;;) {
		if (receive_packet(adb, &packet) < 0)
			return -1;
		if (packet.command == CNXN)
			return 0;
	}
}

static int proxy_client(int client, unsigned long modem_port)
{
	const uint32_t local_id = 1;
	struct adb_packet packet;
	unsigned char client_buffer[CLIENT_BUFFER];
	size_t client_len = 0;
	uint32_t remote_id = 0;
	int waiting_ack = 0;
	int adb = connect_bridge();
	char service[32];

	if (adb < 0 || wait_for_cnxn(adb) < 0)
		goto fail;
	if (snprintf(service, sizeof(service), "tcp:%lu", modem_port) >=
		(int)sizeof(service))
		goto fail;
	if (send_packet(adb, OPEN, local_id, 0, (const unsigned char *)service,
		(uint32_t)strlen(service) + 1) < 0)
		goto fail;

	/* Wait for the remote service to accept the OPEN. */
	for (;;) {
		if (receive_packet(adb, &packet) < 0)
			goto fail;
		if (packet.command == CNXN)
			continue; /* The RM505Q sometimes emits a duplicate greeting. */
		if (packet.command == OKAY && packet.arg1 == local_id) {
			remote_id = packet.arg0;
			break;
		}
		if (packet.command == WRTE && packet.arg1 == local_id) {
			if (send_packet(adb, OKAY, local_id, packet.arg0, NULL, 0) < 0)
				goto fail;
			continue;
		}
		goto fail;
	}

	for (;;) {
		struct pollfd fds[2];
		int ret;
		fds[0].fd = adb;
		fds[0].events = POLLIN;
		fds[1].fd = client;
		fds[1].events = waiting_ack ? 0 : POLLIN;
		ret = poll(fds, 2, 30000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		if (ret == 0)
			continue;
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (receive_packet(adb, &packet) < 0)
				goto fail;
			if (packet.command == CNXN)
				continue;
			if (packet.command == OKAY && packet.arg0 == remote_id &&
				packet.arg1 == local_id) {
				waiting_ack = 0;
			} else if (packet.command == WRTE && packet.arg1 == local_id) {
				if (send_packet(adb, OKAY, local_id, packet.arg0, NULL, 0) < 0 ||
					write_all(client, packet.payload, packet.length) < 0)
					goto fail;
			} else if (packet.command == CLSE && packet.arg1 == local_id) {
				(void)send_packet(adb, CLSE, local_id, packet.arg0, NULL, 0);
				break;
			} else {
				goto fail;
			}
		}
		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t n = recv(client, client_buffer + client_len,
				sizeof(client_buffer) - client_len, 0);
			if (n <= 0)
				break;
			client_len += (size_t)n;
		}
		if (client_len && !waiting_ack) {
			size_t chunk = client_len > MAX_PAYLOAD ? MAX_PAYLOAD : client_len;
			if (send_packet(adb, WRTE, local_id, remote_id, client_buffer,
				(uint32_t)chunk) < 0)
				goto fail;
			memmove(client_buffer, client_buffer + chunk, client_len - chunk);
			client_len -= chunk;
			waiting_ack = 1;
		}
	}
	(void)send_packet(adb, CLSE, local_id, remote_id, NULL, 0);
	close(adb);
	return 0;

fail:
	if (adb >= 0)
		close(adb);
	return -1;
}

int main(int argc, char **argv)
{
	struct sockaddr_in address;
	unsigned long listen_port;
	unsigned long modem_port;
	int listener;
	int reuse = 1;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s <LAN-IP> <LAN-port> <modem-port>\n", argv[0]);
		return 2;
	}
	listen_port = strtoul(argv[2], NULL, 10);
	modem_port = strtoul(argv[3], NULL, 10);
	if (!listen_port || listen_port > 65535 || !modem_port || modem_port > 65535) {
		fprintf(stderr, "Ports must be in the range 1-65535\n");
		return 2;
	}
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)listen_port);
	if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
		fprintf(stderr, "Invalid IPv4 bind address: %s\n", argv[1]);
		close(listener);
		return 2;
	}
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
		listen(listener, 4) < 0) {
		perror("bind/listen");
		close(listener);
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	/*
	 * A browser opens several HTTPS connections at once for QManager's page
	 * assets and API calls.  The console is a long-lived WebSocket, so handling
	 * accepted clients serially leaves it queued behind an ordinary request and
	 * the browser closes it with code 1006.  Each ADB connection gets its own
	 * MHI file descriptor, so handling each TCP client in a small child is safe.
	 */
	signal(SIGCHLD, SIG_IGN);
	fprintf(stderr, "QManager LAN proxy listening on %s:%lu -> modem tcp:%lu\n",
		argv[1], listen_port, modem_port);
	for (;;) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			close(client);
			continue;
		}
		if (pid == 0) {
			close(listener);
			if (proxy_client(client, modem_port) < 0)
				fprintf(stderr, "QManager proxy connection closed\n");
			close(client);
			_exit(0);
		}
		close(client);
	}
	close(listener);
	return 1;
}
