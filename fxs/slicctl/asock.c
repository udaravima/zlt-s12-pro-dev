/* AudioSocket framing helpers; see asock.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "asock.h"

int as_uuid_parse(const char *s, uint8_t out[16])
{
	int i = 0;

	while (*s && i < 32) {
		int v;

		if (*s == '-') {
			s++;
			continue;
		}
		if (*s >= '0' && *s <= '9')
			v = *s - '0';
		else if ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'f')
			v = (*s | 0x20) - 'a' + 10;
		else
			return -1;
		if (i & 1)
			out[i / 2] |= v;
		else
			out[i / 2] = v << 4;
		i++;
		s++;
	}
	return i == 32 && !*s ? 0 : -1;
}

int as_send(int fd, uint8_t kind, const void *payload, uint16_t len)
{
	uint8_t frame[3 + 2048];

	if (len > sizeof(frame) - 3)
		return -1;
	frame[0] = kind;
	frame[1] = len >> 8;
	frame[2] = len & 0xFF;
	if (len)
		memcpy(frame + 3, payload, len);
	/* one send per frame; a short send would desync the stream */
	return send(fd, frame, 3 + len, MSG_NOSIGNAL) == 3 + len ? 0 : -1;
}

int as_recv(int fd, struct as_rx *r)
{
	for (;;) {
		ssize_t n;

		if (r->hlen < 3) {
			n = recv(fd, r->hdr + r->hlen, 3 - r->hlen, 0);
			if (n == 0)
				return -1;
			if (n < 0)
				return errno == EAGAIN || errno == EINTR ? 0 : -1;
			r->hlen += n;
			if (r->hlen < 3)
				continue;
			r->len = (r->hdr[1] << 8) | r->hdr[2];
			r->got = 0;
			r->skip = r->len > (int)sizeof(r->buf) ? r->len : 0;
		}
		if (r->skip) {
			uint8_t junk[512];

			n = recv(fd, junk, r->skip < (int)sizeof(junk) ? r->skip : (int)sizeof(junk), 0);
			if (n == 0)
				return -1;
			if (n < 0)
				return errno == EAGAIN || errno == EINTR ? 0 : -1;
			r->skip -= n;
			if (!r->skip)
				r->hlen = 0;	/* dropped; read the next frame */
			continue;
		}
		if (r->got < r->len) {
			n = recv(fd, r->buf + r->got, r->len - r->got, 0);
			if (n == 0)
				return -1;
			if (n < 0)
				return errno == EAGAIN || errno == EINTR ? 0 : -1;
			r->got += n;
			if (r->got < r->len)
				continue;
		}
		r->hlen = 0;	/* frame complete; next call starts a new one */
		return 1;
	}
}

int as_listen(const char *hostport)
{
	char host[64];
	const char *colon = strrchr(hostport, ':');
	struct sockaddr_in sa = { .sin_family = AF_INET };
	int fd, one = 1;

	if (!colon || colon - hostport >= (int)sizeof(host))
		return -1;
	memcpy(host, hostport, colon - hostport);
	host[colon - hostport] = 0;
	sa.sin_port = htons(atoi(colon + 1));
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || !sa.sin_port)
		return -1;
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 2)) {
		close(fd);
		return -1;
	}
	return fd;
}
