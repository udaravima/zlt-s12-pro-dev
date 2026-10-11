/*
 * Caller ID signal generator: 8 kHz signed 16-bit audio for /dev/slicpcm.
 *
 * The Le9641 can generate caller ID itself, but that needs the VP-API
 * sequencer (vp886_seq.c), which the public "lite" API lacks. So the signal
 * is synthesised here and played over the PCM bus while the line is in
 * on-hook transmission (VP_LINE_OHT).
 *
 * FSK (1200 baud, phase-continuous):
 *   bell: Bell 202 tones, mark 1200 Hz / space 2200 Hz (Telcordia GR-30)
 *   v23:  ITU V.23 tones, mark 1300 Hz / space 2100 Hz (ETSI EN 300 659)
 * Frame: 300 bits channel seizure (0101...), 180 mark bits, then each
 * message byte as start bit 0, 8 data bits LSB first, stop bit 1.
 * Message: MDMF (0x80) with date/time (0x01, "MMDDHHMM"), number (0x02),
 * optional name (0x07), and a checksum making the byte sum 0 mod 256.
 *
 * DTMF CID: "D" + number + "C", 70 ms tone / 70 ms gap per digit.
 */
#include <math.h>
#include <string.h>
#include <time.h>
#include "cid.h"

#define RATE 8000.0

int cid_build_mdmf(uint8_t *msg, int max, const char *number, const char *name)
{
	char dt[16];
	time_t t = time(NULL);
	struct tm tm;
	int n = 2, nl = strlen(number), ml = name ? strlen(name) : 0, i;
	unsigned sum = 0;

	localtime_r(&t, &tm);
	snprintf(dt, sizeof(dt), "%02u%02u%02u%02u", (unsigned)(tm.tm_mon + 1) % 100u,
		 (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u, (unsigned)tm.tm_min % 100u);
	if (nl > 20)
		nl = 20;
	if (ml > 15)
		ml = 15;
	if (2 + 10 + 2 + nl + (ml ? 2 + ml : 0) + 1 > max)
		return -1;
	msg[0] = 0x80;				/* MDMF */
	msg[n++] = 0x01; msg[n++] = 8;		/* date/time */
	memcpy(msg + n, dt, 8); n += 8;
	msg[n++] = 0x02; msg[n++] = nl;		/* calling number */
	memcpy(msg + n, number, nl); n += nl;
	if (ml) {
		msg[n++] = 0x07; msg[n++] = ml;	/* calling name */
		memcpy(msg + n, name, ml); n += ml;
	}
	msg[1] = n - 2;				/* length of the parameters */
	for (i = 0; i < n; i++)
		sum += msg[i];
	msg[n++] = (uint8_t)(0x100 - (sum & 0xFF));	/* checksum */
	return n;
}

struct fsk {
	int16_t *out;
	int max, len;
	double phase, mark, space, amp, bit_clock;
};

static void fsk_bit(struct fsk *f, int bit)
{
	double freq = bit ? f->mark : f->space;

	/* 1200 baud at 8 kHz = 6.667 samples/bit: carry the fraction over */
	f->bit_clock += RATE / 1200.0;
	while (f->bit_clock >= 1.0 && f->len < f->max) {
		f->out[f->len++] = (int16_t)(f->amp * 32767.0 * sin(f->phase));
		f->phase += 2 * M_PI * freq / RATE;
		if (f->phase > 2 * M_PI)
			f->phase -= 2 * M_PI;
		f->bit_clock -= 1.0;
	}
}

int cid_fsk(int16_t *out, int max, const uint8_t *msg, int len, int v23, double amp)
{
	struct fsk f = { out, max, 0, 0, v23 ? 1300 : 1200, v23 ? 2100 : 2200, amp, 0 };
	int i, b;

	for (i = 0; i < 300; i++)
		fsk_bit(&f, i & 1);		/* channel seizure 0101... */
	for (i = 0; i < 180; i++)
		fsk_bit(&f, 1);			/* mark */
	for (i = 0; i < len; i++) {
		fsk_bit(&f, 0);			/* start */
		for (b = 0; b < 8; b++)
			fsk_bit(&f, (msg[i] >> b) & 1);
		fsk_bit(&f, 1);			/* stop */
	}
	for (i = 0; i < 10; i++)
		fsk_bit(&f, 1);			/* trailing mark */
	return f.len;
}

static const char *DTMF_KEYS = "123A456B789C*0#D";
static const int DTMF_LOW[] = { 697, 770, 852, 941 };
static const int DTMF_HIGH[] = { 1209, 1336, 1477, 1633 };

int cid_dtmf(int16_t *out, int max, const char *number, double amp)
{
	char seq[32];
	int n = 0, i, k, s;

	snprintf(seq, sizeof(seq), "D%sC", number);
	for (i = 0; seq[i]; i++) {
		const char *p = strchr(DTMF_KEYS, seq[i]);

		if (!p)
			continue;
		k = p - DTMF_KEYS;
		for (s = 0; s < 560 && n < max; s++)	/* 70 ms tone */
			out[n++] = (int16_t)(amp * 0.5 * 32767.0 *
				(sin(2 * M_PI * DTMF_LOW[k / 4] * s / RATE) +
				 sin(2 * M_PI * DTMF_HIGH[k % 4] * s / RATE)));
		for (s = 0; s < 560 && n < max; s++)	/* 70 ms gap */
			out[n++] = 0;
	}
	return n;
}
