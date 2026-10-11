#ifndef SLICCTL_CID_H
#define SLICCTL_CID_H
#include <stdint.h>
#include <stdio.h>

/* Build an MDMF caller-ID message (date/time, number, optional name,
   checksum). Returns its length, or -1 if it doesn't fit. */
int cid_build_mdmf(uint8_t *msg, int max, const char *number, const char *name);

/* 1200-baud FSK burst (seizure, marks, framed bytes) as 8 kHz s16 samples.
   v23 = 0: Bell 202 tones; 1: V.23 tones. Returns the number of samples. */
int cid_fsk(int16_t *out, int max, const uint8_t *msg, int len, int v23, double amp);

/* DTMF caller ID "D<number>C" as 8 kHz s16 samples. */
int cid_dtmf(int16_t *out, int max, const char *number, double amp);

#endif
