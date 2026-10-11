/* AudioSocket (Asterisk) framing: [kind:1][length:2, big endian][payload].
 * Kinds we use: 0x00 hangup, 0x01 UUID (16 raw bytes), 0x03 DTMF (from
 * Asterisk only; sending one back makes Asterisk drop the call), 0x10 audio
 * (16-bit 8 kHz mono, little endian), 0xFF error. */
#ifndef ASOCK_H
#define ASOCK_H
#include <stdint.h>

#define AS_HANGUP	0x00
#define AS_UUID		0x01
#define AS_DTMF		0x03
#define AS_AUDIO	0x10
#define AS_ERROR	0xFF

struct as_rx {
	uint8_t hdr[3];
	int hlen;		/* header bytes collected */
	uint8_t buf[4096];
	int len, got;		/* payload length and bytes collected */
	int skip;		/* oversize payload: bytes still to discard */
};

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> 16 bytes; 0 on success */
int as_uuid_parse(const char *s, uint8_t out[16]);
/* send one frame; 0 on success, -1 if the socket is gone or full */
int as_send(int fd, uint8_t kind, const void *payload, uint16_t len);
/* Pull bytes from fd into r. Returns 1 when a whole frame is ready (kind in
 * r->hdr[0], payload r->buf[0..r->len)), 0 when more bytes are needed,
 * -1 on EOF/error. Call again after a frame until it returns 0. */
int as_recv(int fd, struct as_rx *r);
/* listening TCP socket for "host:port" (non-blocking); -1 on error */
int as_listen(const char *hostport);

#endif
