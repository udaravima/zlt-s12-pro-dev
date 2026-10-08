#!/usr/bin/env python3
"""Analyse a raw S16LE 8 kHz capture from /dev/slicpcm.

Prints level, dominant frequency and tone purity (fraction of signal energy
within +-30 Hz of the peak) for the capture as read and for alternative
interpretations of the sample packing (pair swap, byte swap). A correctly
aligned loopback of a single tone gives one clean peak with high purity.
"""
import struct
import sys

import numpy as np

RATE = 8000


def report(name, x):
    x = x.astype(np.float64)
    x -= x.mean()
    rms = np.sqrt(np.mean(x * x))
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1 / RATE)
    band = (freqs > 100) & (freqs < 3900)
    k = np.argmax(np.where(band, spec, 0))
    near = np.abs(freqs - freqs[k]) <= 30
    purity = spec[near].sum() / max(spec[band].sum(), 1e-12)
    top = np.argsort(np.where(band, spec, 0))[::-1]
    peaks = []
    for i in top:
        if all(abs(freqs[i] - p) > 40 for p in peaks):
            peaks.append(freqs[i])
        if len(peaks) == 4:
            break
    print(f"{name:12s} rms={rms:8.1f} ({20*np.log10(max(rms,1e-9)/32768):6.1f} dBFS)"
          f"  peak={freqs[k]:7.1f} Hz  purity={purity*100:5.1f}%"
          f"  top={', '.join(f'{p:.0f}' for p in peaks)}")


def main(path):
    raw = open(path, 'rb').read()
    raw = raw[: len(raw) // 4 * 4]
    s = np.array(struct.unpack('<%dh' % (len(raw) // 2), raw), dtype=np.int16)
    report('as-read', s)
    report('pair-swap', s.reshape(-1, 2)[:, ::-1].reshape(-1))
    report('byte-swap', s.byteswap())
    report('even-only', s[0::2])
    report('odd-only', s[1::2])


if __name__ == '__main__':
    main(sys.argv[1])
