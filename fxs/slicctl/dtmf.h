/* DTMF (keypad tone) detector for 8 kHz 16-bit audio: 8 Goertzel filters
 * over 102-sample blocks (12.75 ms), the block size Asterisk's own detector
 * uses: short enough to catch 40 ms tones, wide enough (78 Hz bins) to
 * accept keys 1.5 % off frequency. */
#ifndef DTMF_H
#define DTMF_H
#include <stdint.h>

#define DTMF_BLOCK 102

struct dtmf_det {
	double q1[8], q2[8], energy;	/* filter state + block energy */
	int n;				/* samples in the current block */
	char cand;			/* digit seen in the previous block */
	int same;			/* consecutive blocks agreeing on cand */
	char held;			/* digit currently reported as pressed */
	int quiet;			/* consecutive blocks without held */
};

void dtmf_init(struct dtmf_det *d);
/* Feed samples; writes each newly pressed key (0-9 * # A-D) to out and
 * returns how many (a key is reported once, when confirmed in 2 blocks). */
int dtmf_feed(struct dtmf_det *d, const int16_t *s, int n, char *out, int max);

#endif
