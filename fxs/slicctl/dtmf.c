/*
 * DTMF detector. Each block of 102 samples runs one Goertzel filter per
 * DTMF frequency (78 Hz wide at 8 kHz: 697 Hz leaks into the 770 Hz filter
 * at -22 dB, and the 400-450 Hz dial tone that echoes back from the line
 * while the user dials is 3 bins away). A block "hears" a key when:
 *   - the strongest row tone and strongest column tone are each loud enough,
 *   - each beats the runner-up in its group by 6 dB (no chords),
 *   - their levels are within 8 dB of each other (twist),
 *   - together they carry most of the block's energy (not speech/noise).
 * A key is reported after 2 agreeing blocks (25.5 ms, so a 40 ms tone is
 * caught however it falls across block edges) and re-armed after 2 blocks
 * without it, so holding a key yields one digit and pressing the same key
 * twice yields two.
 */
#include <math.h>
#include <string.h>
#include "dtmf.h"

static const double freq[8] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
static const char keys[4][4] = {
	{ '1', '2', '3', 'A' }, { '4', '5', '6', 'B' },
	{ '7', '8', '9', 'C' }, { '*', '0', '#', 'D' },
};
static double coef[8];

#define MIN_AMP		150.0	/* per tone, of 32767 full scale (about -47 dBFS) */
#define TWIST		6.3	/* power ratio, 8 dB */
#define DOMINANCE	4.0	/* power ratio, 6 dB */
#define PURITY		0.55	/* share of block energy in the two tones */

void dtmf_init(struct dtmf_det *d)
{
	int i;

	memset(d, 0, sizeof(*d));
	if (coef[0] == 0)
		for (i = 0; i < 8; i++)
			coef[i] = 2 * cos(2 * M_PI * freq[i] / 8000.0);
}

/* the key heard in the block just completed, or 0 */
static char block_key(struct dtmf_det *d)
{
	double p[8], tot = d->energy;
	int i, r = 0, c = 4;

	for (i = 0; i < 8; i++)
		p[i] = d->q1[i] * d->q1[i] + d->q2[i] * d->q2[i] - coef[i] * d->q1[i] * d->q2[i];
	for (i = 1; i < 4; i++)
		if (p[i] > p[r])
			r = i;
	for (i = 5; i < 8; i++)
		if (p[i] > p[c])
			c = i;
	/* amplitude of a pure tone A over N samples: p = (A N / 2)^2 */
	if (2 * sqrt(p[r]) / DTMF_BLOCK < MIN_AMP || 2 * sqrt(p[c]) / DTMF_BLOCK < MIN_AMP)
		return 0;
	if (p[r] > TWIST * p[c] || p[c] > TWIST * p[r])
		return 0;
	for (i = 0; i < 4; i++)
		if (i != r && p[i] * DOMINANCE > p[r])
			return 0;
	for (i = 4; i < 8; i++)
		if (i != c && p[i] * DOMINANCE > p[c])
			return 0;
	/* a tone's share of block energy: p / (N/2 * sum s^2) */
	if ((p[r] + p[c]) / (DTMF_BLOCK / 2.0 * tot) < PURITY)
		return 0;
	return keys[r][c - 4];
}

int dtmf_feed(struct dtmf_det *d, const int16_t *s, int n, char *out, int max)
{
	int got = 0, i, k;

	for (k = 0; k < n; k++) {
		double x = s[k];

		for (i = 0; i < 8; i++) {
			double q0 = coef[i] * d->q1[i] - d->q2[i] + x;

			d->q2[i] = d->q1[i];
			d->q1[i] = q0;
		}
		d->energy += x * x;
		if (++d->n < DTMF_BLOCK)
			continue;

		char key = block_key(d);

		memset(d->q1, 0, sizeof(d->q1));
		memset(d->q2, 0, sizeof(d->q2));
		d->energy = 0;
		d->n = 0;

		d->same = key && key == d->cand ? d->same + 1 : 1;
		d->cand = key;
		if (key && key == d->held) {
			d->quiet = 0;
		} else if (++d->quiet >= 2) {
			d->held = 0;
		}
		if (key && !d->held && d->same >= 2) {
			d->held = key;
			d->quiet = 0;
			if (got < max)
				out[got++] = key;
		}
	}
	return got;
}
