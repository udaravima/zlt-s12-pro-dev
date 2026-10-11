#!/usr/bin/env python3
"""Decode caller ID from raw S16LE 8 kHz audio (a generated file or a PCM
capture from /dev/slicpcm). Independent of the generator in slicctl/cid.c.

    cid_decode.py FILE.s16 [bell|v23|dtmf]

FSK: per-sample tone correlation over one bit period, UART framing
(start 0, 8 data LSB first, stop 1), then MDMF/SDMF parsing with checksum.
DTMF: Goertzel per 10 ms block, digit runs separated by silence.
"""
import sys

import numpy as np

RATE = 8000
BIT = RATE / 1200.0


def load(path):
    return np.frombuffer(open(path, 'rb').read(), dtype='<i2').astype(np.float64)


def fsk_bits(x, mark, space):
    n = np.arange(len(x))
    win = np.ones(int(round(BIT)))

    def env(f):
        c = np.convolve(x * np.cos(2 * np.pi * f * n / RATE), win, 'same')
        s = np.convolve(x * np.sin(2 * np.pi * f * n / RATE), win, 'same')
        return c * c + s * s
    em, es = env(mark), env(space)
    active = (em + es) > 0.05 * (em + es).max()
    return np.where(em > es, 1, 0), active


def uart(d, active):
    out, i = [], 0
    # skip the seizure + mark preamble: wait for >= 40 bits of steady mark
    run = 0
    while i < len(d):
        run = run + 1 if (d[i] == 1 and active[i]) else 0
        i += 1
        if run > 40 * BIT:
            break
    while i < len(d):
        if d[i] == 0 and d[i - 1] == 1 and active[i]:		# start-bit edge
            centre = i + BIT / 2
            if d[int(centre)] != 0:
                i += 1
                continue
            byte = 0
            for b in range(8):
                byte |= int(d[int(centre + (b + 1) * BIT)]) << b
            stop = d[min(int(centre + 9 * BIT), len(d) - 1)]
            if stop != 1:
                break
            out.append(byte)
            i = int(centre + 9 * BIT)
        else:
            i += 1
    return bytes(out)


def parse(msg):
    if len(msg) < 3:
        return f'no message ({len(msg)} bytes)'
    total = msg[1] + 3
    body = msg[:total]
    ok = (sum(body) & 0xFF) == 0
    res = [f'type 0x{msg[0]:02X} ({"MDMF" if msg[0] == 0x80 else "SDMF" if msg[0] == 0x04 else "?"}), '
           f'{msg[1]} bytes, checksum {"OK" if ok else "BAD"}']
    if msg[0] == 0x80:
        p = 2
        names = {1: 'date/time', 2: 'number', 4: 'no-number reason', 7: 'name', 8: 'no-name reason'}
        while p < total - 1:
            t, ln = msg[p], msg[p + 1]
            res.append(f'  {names.get(t, hex(t))}: {msg[p + 2:p + 2 + ln].decode(errors="replace")}')
            p += 2 + ln
    elif msg[0] == 0x04:
        res.append(f'  date/time: {msg[2:10].decode()}  number: {msg[10:total - 1].decode()}')
    return '\n'.join(res)


def dtmf(x):
    keys = '123A456B789C*0#D'
    lows, highs = [697, 770, 852, 941], [1209, 1336, 1477, 1633]
    blk, digits, last = 80, [], None
    for s in range(0, len(x) - blk, blk):
        seg = x[s:s + blk]

        def g(f):
            k = 2 * np.cos(2 * np.pi * f / RATE)
            a = b = 0.0
            for v in seg:
                a, b = v + k * a - b, a
            return a * a + b * b - k * a * b
        lo = [g(f) for f in lows]
        hi = [g(f) for f in highs]
        if max(lo) + max(hi) < 1e9:
            last = None
            continue
        key = keys[int(np.argmax(lo)) * 4 + int(np.argmax(hi))]
        if key != last:
            digits.append(key)
        last = key
    return ''.join(digits)


def main():
    x = load(sys.argv[1])
    fmt = sys.argv[2] if len(sys.argv) > 2 else 'bell'
    if fmt == 'dtmf':
        print('DTMF:', dtmf(x))
        return
    mark, space = (1300, 2100) if fmt == 'v23' else (1200, 2200)
    d, active = fsk_bits(x, mark, space)
    msg = uart(d, active)
    print(f'{fmt}: {len(msg)} bytes: {msg.hex(" ")}')
    print(parse(msg))


if __name__ == '__main__':
    main()
