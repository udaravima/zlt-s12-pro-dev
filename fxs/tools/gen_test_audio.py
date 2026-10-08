#!/usr/bin/env python3
"""Write raw S16LE 8 kHz mono test files for /dev/slicpcm.

    gen_test_audio.py <outdir>

tone_1khz_5s.s16  - steady 1 kHz at -10 dBFS: pitch/level reference
scale_c4_c5.s16   - C major scale, 0.4 s per note: sounds wrong at once if
                    samples are dropped, doubled or the rate is off

To play your own clip, convert it on a PC first, e.g.
    ffmpeg -i clip.mp3 -ac 1 -ar 8000 -f s16le clip.s16
"""
import math
import os
import struct
import sys

RATE = 8000


def tone(freq, sec, amp=0.3):
    return [int(amp * 32767 * math.sin(2 * math.pi * freq * i / RATE)) for i in range(int(RATE * sec))]


def save(path, samples):
    with open(path, 'wb') as f:
        f.write(struct.pack('<%dh' % len(samples), *samples))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    save(os.path.join(out, 'tone_1khz_5s.s16'), tone(1000, 5))
    scale = []
    for f in (262, 294, 330, 349, 392, 440, 494, 523):
        scale += tone(f, 0.4) + [0] * int(RATE * 0.05)
    save(os.path.join(out, 'scale_c4_c5.s16'), scale)
    print('wrote tone_1khz_5s.s16, scale_c4_c5.s16 to', out)


if __name__ == '__main__':
    main()
