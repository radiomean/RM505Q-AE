/*
 * mhi-adb-bridge: make a non-TCP Qualcomm MHI ADB channel available to adb.
 *
 * Listens on 127.0.0.1 only.  Use SSH local forwarding from a trusted host;
 * do not bind this service to a LAN/WAN address.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 16384
#define ADB_HEADER_SIZE 24
#define MHI_PENDING_SIZE 65536
#define ERESTARTSYS_ERRNO 512

static int write_all(int fd, const unsigned char *buf, size_t len)
{
	while (len) {
		ssize_t written = write(fd, buf, len);
		if (written > 0) {
			buf += written;
			len -= (size_t)written;
			continue;
		}
		if (written < 0 && (errno == EINTR || errno == EAGAIN ||
					   errno == ERESTARTSYS_ERRNO)) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			if (poll(&pfd, 1, 5000) > 0)
				continue;
		}
		return -1;
	}
	return 0;
}

static int send_all(int fd, const unsigned char *buf, size_t len)
{
	while (len) {
		ssize_t written = send(fd, buf, len, MSG_NOSIGNAL);
		if (written > 0) {
			buf += written;
			len -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

/*
 * Quectel's MHI ADB endpoint can deliver output from a just-closed previous
 * session after the next TCP client is accepted.  Passing that output to adb
 * before the fresh CNXN reply makes adb mark the transport offline.  Let the
 * endpoint settle for one second, discarding only pre-handshake packets.
 */
static int drain_mhi(int fd, int window_ms)
{
	unsigned char buf[BUF_SIZE];
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int total = 0;
	int ret;
	int waited = 0;

	/*
	 * The RM505Q only replies with its CNXN after the host has sent one, so a
	 * bounded pre-handshake drain cannot consume the next valid greeting.
	 * Poll in short chunks: stale packets can arrive just after open().
	 */
	while (waited < window_ms) {
		ret = poll(&pfd, 1, 100);
		waited += 100;
		if (ret == 0)
			continue;
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		ssize_t received = read(fd, buf, sizeof(buf));
		if (received > 0) {
			total += (int)received;
			continue;
		}
		if (received < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		break;
	}
	return total;
}

static void log_frame(const char *direction, const unsigned char *buf, ssize_t len)
{
	unsigned int payload_len = 0;
	ssize_t i;
	if (len >= 4 && buf[0] >= 32 && buf[0] <= 126 && buf[1] >= 32 &&
	    buf[1] <= 126 && buf[2] >= 32 && buf[2] <= 126 && buf[3] >= 32 &&
	    buf[3] <= 126) {
		fprintf(stderr, "%s %zd byte(s), starts %.4s", direction, len, buf);
		if (len >= ADB_HEADER_SIZE) {
			payload_len = (unsigned int)buf[12] |
				((unsigned int)buf[13] << 8) |
				((unsigned int)buf[14] << 16) |
				((unsigned int)buf[15] << 24);
			if (payload_len && payload_len <= (unsigned int)(len - ADB_HEADER_SIZE)) {
				fprintf(stderr, " payload='");
				for (i = 0; i < (ssize_t)payload_len && i < 96; i++) {
					unsigned char c = buf[ADB_HEADER_SIZE + i];
					fputc(c >= 32 && c <= 126 ? c : '.', stderr);
				}
				fprintf(stderr, "'");
			}
		}
		fputc('\n', stderr);
	}
	else
		fprintf(stderr, "%s %zd byte(s)\n", direction, len);
	fflush(stderr);
}

static unsigned int get_le32(const unsigned char *buf)
{
	return (unsigned int)buf[0] | ((unsigned int)buf[1] << 8) |
		((unsigned int)buf[2] << 16) | ((unsigned int)buf[3] << 24);
}

static void put_le32(unsigned char *buf, unsigned int value)
{
	buf[0] = (unsigned char)value;
	buf[1] = (unsigned char)(value >> 8);
	buf[2] = (unsigned char)(value >> 16);
	buf[3] = (unsigned char)(value >> 24);
}

/* ADB flow-control reply for a packet that belongs to a discarded session. */
static int acknowledge_stale_packet(int mhi, const unsigned char *packet)
{
	unsigned char reply[ADB_HEADER_SIZE] = { 0 };
	unsigned int command = get_le32(packet);

	if (!memcmp(packet, "WRTE", 4)) {
		memcpy(reply, "OKAY", 4);
		put_le32(reply + 4, get_le32(packet + 8));
		put_le32(reply + 8, get_le32(packet + 4));
	} else if (!memcmp(packet, "CLSE", 4)) {
		memcpy(reply, "CLSE", 4);
		put_le32(reply + 4, get_le32(packet + 8));
		put_le32(reply + 8, get_le32(packet + 4));
	} else {
		return 0;
	}
	put_le32(reply + 20, get_le32(reply) ^ 0xffffffffU);
	(void)command;
	return write_all(mhi, reply, sizeof(reply));
}

/*
 * TCP does not preserve ADB message boundaries, so reassemble complete ADB
 * frames first.  The RM505Q MHI ADB endpoint has one non-standard detail:
 * its working native transport sends an ADB header and its payload as two
 * separate MHI writes.  It ACKs a combined OPEN but may never execute it.
 */
static int forward_tcp_packets(int mhi, unsigned char *pending,
			       size_t *pending_len, unsigned char *last_open,
			       size_t *last_open_len, unsigned char *last_cnxn,
			       size_t *last_cnxn_len, int *open_forwarded)
{
	while (*pending_len >= ADB_HEADER_SIZE) {
		unsigned int payload_len = get_le32(pending + 12);
		size_t packet_len = ADB_HEADER_SIZE + (size_t)payload_len;

		if (payload_len > MHI_PENDING_SIZE - ADB_HEADER_SIZE ||
		    *pending_len < packet_len)
			return payload_len > MHI_PENDING_SIZE - ADB_HEADER_SIZE ? -1 : 0;
		if (write_all(mhi, pending, ADB_HEADER_SIZE) < 0)
			return -1;
		if (payload_len && write_all(mhi, pending + ADB_HEADER_SIZE,
					     payload_len) < 0)
			return -1;
		if (!memcmp(pending, "OPEN", 4)) {
			memcpy(last_open, pending, packet_len);
			*last_open_len = packet_len;
			*open_forwarded = 1;
		}
		if (!memcmp(pending, "CNXN", 4)) {
			memcpy(last_cnxn, pending, packet_len);
			*last_cnxn_len = packet_len;
		}
		log_frame("ADB TCP -> MHI", pending, (ssize_t)packet_len);
		*pending_len -= packet_len;
		memmove(pending, pending + packet_len, *pending_len);
	}
	return 0;
}

/*
 * The RM505Q occasionally emits a second delayed CNXN packet after its first
 * greeting.  The first establishes the transport; later copies are stale
 * channel-open notifications and reset adb if forwarded.  Reassemble MHI
 * reads into complete ADB packets so duplicates can be suppressed safely.
 */
static int forward_mhi_packets(int client, int mhi, unsigned char *pending,
			       size_t *pending_len, int *sent_cnxn,
			       int *new_cnxn, int *duplicate_cnxn,
			       int *service_reply, const unsigned char *last_cnxn,
			       size_t last_cnxn_len)
{
	while (*pending_len >= ADB_HEADER_SIZE) {
		unsigned int payload_len = get_le32(pending + 12);
		size_t packet_len = ADB_HEADER_SIZE + (size_t)payload_len;
		int is_cnxn;
		int is_clse;

		if (payload_len > BUF_SIZE || *pending_len < packet_len)
			return payload_len > BUF_SIZE ? -1 : 0;
		is_cnxn = !memcmp(pending, "CNXN", 4);
		is_clse = !memcmp(pending, "CLSE", 4);
		/*
		 * A command abandoned by a timed-out TCP client may finish only when
		 * the next host greeting arrives.  Its OKAY/WRTE/CLSE frames precede
		 * the device CNXN and must never be shown to the new adb client.
		 */
		if (!*sent_cnxn && !is_cnxn) {
			log_frame("Discarded stale MHI -> ADB TCP", pending,
				(ssize_t)packet_len);
			/*
			 * Hiding old output from a new TCP client is not enough: the modem's
			 * ADB daemon will not emit its next frame until every WRTE/CLSE has
			 * been acknowledged.  Drain that old session at the ADB protocol
			 * level before asking it to greet the new client.
			 */
			if (acknowledge_stale_packet(mhi, pending) < 0)
				return -1;
			if (is_clse && last_cnxn_len) {
				if (write_all(mhi, last_cnxn, last_cnxn_len) < 0)
					return -1;
				fprintf(stderr, "Re-sent host CNXN after stale service close\n");
				fflush(stderr);
			}
			*pending_len -= packet_len;
			memmove(pending, pending + packet_len, *pending_len);
			continue;
		}
		if (is_cnxn && *sent_cnxn) {
			fprintf(stderr, "Discarded delayed duplicate modem CNXN (%zu byte(s))\n",
				packet_len);
			fflush(stderr);
			*duplicate_cnxn = 1;
		} else {
			if (send_all(client, pending, packet_len) < 0)
				return -1;
			log_frame("MHI -> ADB TCP", pending, (ssize_t)packet_len);
			if (is_cnxn) {
				*sent_cnxn = 1;
				*new_cnxn = 1;
			} else {
				*service_reply = 1;
			}
		}
		*pending_len -= packet_len;
		memmove(pending, pending + packet_len, *pending_len);
	}
	return 0;
}

static void bridge_client(int client, const char *device)
{
	int mhi;
	unsigned char buf[BUF_SIZE];
	unsigned char host_pending[MHI_PENDING_SIZE];
	unsigned char last_open[BUF_SIZE];
	unsigned char last_cnxn[BUF_SIZE];
	unsigned char pending[MHI_PENDING_SIZE];
	size_t host_pending_len = 0;
	size_t pending_len = 0;
	size_t last_open_len = 0;
	size_t last_cnxn_len = 0;
	int sent_cnxn = 0;
	int open_pending = 0;
	int open_retry_sent = 0;

	/* The RM505Q accepts one host ADB session per MHI UCI file handle. */
	mhi = open(device, O_RDWR | O_NONBLOCK);
	if (mhi < 0) {
		perror(device);
		return;
	}
	fprintf(stderr, "ADB client connected; startup drain: %d stale byte(s)\n",
		drain_mhi(mhi, 1000));
	fflush(stderr);

	for (;;) {
		fd_set readable;
		struct timeval timeout;
		struct timeval *timeout_ptr = NULL;
		int maxfd = client > mhi ? client : mhi;
		int ret;
		ssize_t n;

		FD_ZERO(&readable);
		FD_SET(client, &readable);
		FD_SET(mhi, &readable);
		if (open_pending && !open_retry_sent) {
			timeout.tv_sec = 1;
			timeout.tv_usec = 0;
			timeout_ptr = &timeout;
		}
		ret = select(maxfd + 1, &readable, NULL, NULL, timeout_ptr);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0) {
			if (last_cnxn_len && write_all(mhi, last_cnxn, last_cnxn_len) == 0) {
				fprintf(stderr, "No reply to OPEN; re-sent host CNXN\n");
				fflush(stderr);
				open_retry_sent = 1;
				continue;
			}
			perror("MHI ADB CNXN retry");
			break;
		}

		if (FD_ISSET(client, &readable)) {
			int open_forwarded = 0;
			n = read(client, buf, sizeof(buf));
			if (n <= 0) {
				fprintf(stderr, "ADB TCP client disconnected\n");
				break;
			}
			if ((size_t)n > sizeof(host_pending) - host_pending_len) {
				fprintf(stderr, "ADB TCP packet buffer overflow\n");
				break;
			}
			memcpy(host_pending + host_pending_len, buf, (size_t)n);
			host_pending_len += (size_t)n;
			if (forward_tcp_packets(mhi, host_pending, &host_pending_len,
						last_open, &last_open_len,
						last_cnxn, &last_cnxn_len,
						&open_forwarded) < 0) {
				perror("MHI ADB write");
				break;
			}
			if (open_forwarded) {
				open_pending = 1;
				open_retry_sent = 0;
			}
		}
		if (FD_ISSET(mhi, &readable)) {
			int new_cnxn = 0;
			int duplicate_cnxn = 0;
			int service_reply = 0;
			n = read(mhi, buf, sizeof(buf));
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (n <= 0) {
				if (n < 0)
					perror("MHI ADB read");
				else
					fprintf(stderr, "MHI ADB channel closed\n");
				break;
			}
			if ((size_t)n > sizeof(pending) - pending_len) {
				fprintf(stderr, "MHI ADB packet buffer overflow\n");
				break;
			}
			memcpy(pending + pending_len, buf, (size_t)n);
			pending_len += (size_t)n;
			if (forward_mhi_packets(client, mhi, pending, &pending_len,
						&sent_cnxn, &new_cnxn,
						&duplicate_cnxn, &service_reply, last_cnxn,
						last_cnxn_len) < 0) {
				perror("ADB TCP write");
				break;
			}
			/*
			 * Once the modem has accepted an OPEN (OKAY or WRTE), that exact
			 * request is no longer eligible for retry.  The MHI endpoint can
			 * still emit a delayed duplicate CNXN at this point; replaying the
			 * cached OPEN after real output would run the shell script twice.
			 */
			if (service_reply) {
				open_pending = 0;
				last_open_len = 0;
			}
			/*
			 * The RM505Q sends a duplicate CNXN just after the valid greeting.
			 * Keep the host's OPEN request queued in its TCP socket briefly, then
			 * discard that duplicate before passing OPEN to the modem.
			 */
			if (new_cnxn) {
				int dropped = drain_mhi(mhi, 500);
				if (dropped) {
					fprintf(stderr, "Settling after CNXN; discarded %d byte(s)\n",
						dropped);
					fflush(stderr);
				}
			}
			/*
			 * A late greeting sometimes consumes an OPEN.  Do not replay it here:
			 * the original may still be executing, and an immediate replay can
			 * run a shell script twice.  The one-second OPEN watchdog sends a new
			 * CNXN only if no service reply arrived; only then is the OPEN replayed.
			 */
			if (duplicate_cnxn && open_retry_sent && last_open_len) {
				if (write_all(mhi, last_open, last_open_len) < 0) {
					perror("MHI ADB OPEN retry");
					break;
				}
				fprintf(stderr, "Replayed host OPEN after delayed CNXN\n");
				fflush(stderr);
				last_open_len = 0;
			}
		}
	}
	close(mhi);

}

int main(int argc, char **argv)
{
	const char *device = "/dev/mhi_ADB";
	unsigned long requested_port = 6555;
	struct sockaddr_in address;
	int listener;
	int reuse = 1;

	if (argc > 1)
		device = argv[1];
	if (argc > 2)
		requested_port = strtoul(argv[2], NULL, 10);
	if (!requested_port || requested_port > 65535) {
		fprintf(stderr, "Usage: %s [/dev/mhi_ADB] [port]\n", argv[0]);
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);
	/*
	 * QManager's browser frontend makes concurrent HTTPS requests and keeps a
	 * WebSocket open for ttyd.  Each connection must receive its own /dev/mhi_ADB
	 * file descriptor; serial accept handling otherwise leaves the WebSocket
	 * queued behind an ordinary request and it fails with browser code 1006.
	 */
	signal(SIGCHLD, SIG_IGN);
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)requested_port);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(listener, 1) < 0) {
		perror("bind/listen");
		close(listener);
		return 1;
	}

	fprintf(stderr, "mhi-adb-bridge listening on 127.0.0.1:%lu -> %s\n",
		requested_port, device);
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
			bridge_client(client, device);
			close(client);
			_exit(0);
		}
		close(client);
	}
	close(listener);
	return 1;
}
