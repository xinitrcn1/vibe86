/* vsim's JTAG probe: an OpenOCD remote_bitbang server (jtag_bitbang.h) */
#include "jtag_bitbang.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

int
jtag_bitbang_open(jtag_bitbang *jb, int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		std::perror("vsim: jtag: socket");
		return -1;
	}
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) {
		std::fprintf(stderr, "vsim: jtag: port %d: %s\n", port, std::strerror(errno));
		close(s);
		return -1;
	}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	jb->listen_fd = s;
	std::fprintf(stderr, "vsim: jtag: remote_bitbang on 127.0.0.1:%d\n", port);
	return 0;
}

static void
drop_client(jtag_bitbang *jb)
{
	if (jb->fd >= 0)
		close(jb->fd);
	jb->fd = -1;
	jb->n = jb->i = 0;
	/* a probe that goes away releases its resets */
	jb->trst = jb->srst = false;
}

/* refill the input buffer; false if nothing is waiting */
static bool
refill(jtag_bitbang *jb)
{
	if (jb->backoff) {
		jb->backoff--;
		return false;
	}
	if (jb->fd < 0) {
		int c = accept(jb->listen_fd, nullptr, nullptr);
		if (c < 0) {
			jb->backoff = 4096;		/* nobody connecting: look rarely */
			return false;
		}
		fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		int one = 1;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		jb->fd = c;
	}
	ssize_t r = recv(jb->fd, jb->buf, sizeof jb->buf, 0);
	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		drop_client(jb);
		return false;
	}
	if (r < 0) {
		jb->backoff = 64;		/* connected but idle */
		return false;
	}
	jb->n = (size_t)r;
	jb->i = 0;
	return true;
}

void
jtag_bitbang_poll(jtag_bitbang *jb, bool tdo)
{
	if (jb->listen_fd < 0)
		return;
	for (;;) {
		if (jb->i == jb->n && !refill(jb))
			return;
		char c = jb->buf[jb->i++];
		if (c >= '0' && c <= '7') {
			int v = c - '0';
			jb->tck = v & 4;
			jb->tms = v & 2;
			jb->tdi = v & 1;
			return;				/* one pin change per cycle */
		}
		if (c >= 'r' && c <= 'u') {
			jb->trst = c == 't' || c == 'u';
			jb->srst = c == 's' || c == 'u';
			return;
		}
		if (c == 'R') {
			char o = tdo ? '1' : '0';
			if (send(jb->fd, &o, 1, 0) != 1)
				drop_client(jb);
		} else if (c == 'Q')
			drop_client(jb);
		/* 'B'/'b' (LED) and anything else: ignored */
	}
}

void
jtag_bitbang_close(jtag_bitbang *jb)
{
	drop_client(jb);
	if (jb->listen_fd >= 0)
		close(jb->listen_fd);
	jb->listen_fd = -1;
}
