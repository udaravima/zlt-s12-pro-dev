#!/usr/bin/env python3
"""Measure the RJ11 line's echo through extension 800 (AudioSocket).

Run this as the AudioSocket app, lift the handset, dial 800 and stay
silent. The app plays a fixed test signal down the line, records what
comes back, then hangs up:

    1.0 s silence          the line's noise floor
    4.0 s noise, -20 dBFS  echo level, delay and impulse response
    1.0 s silence
    4.0 s noise, -10 dBFS  same at a louder level: does the echo stay linear?
    1.0 s silence

and prints, per noise burst:
  ERL     echo return loss: how far below the sent signal the echo is (dB)
  delay   where the echo's impulse response peaks, from the app's side
          (includes Asterisk's and the daemon's queues, not only the line)
  spread  how many ms of the impulse response hold 99 % of its energy:
          the filter length a canceller needs once the bulk delay is removed
  ERLE    how much a perfect linear canceller (least squares, 64 ms) would
          remove: the ceiling for OSLEC on this line

The sent and received audio are saved as echo-<uuid>-tx.wav / -rx.wav.

    python3 echo_measure.py [--listen 0.0.0.0:9092]
"""
import argparse
import asyncio
import struct
import uuid
import wave

import numpy as np

RATE = 8000
FRAME = 320  # bytes per 20 ms
TAPS = 512   # 64 ms window for the impulse response


def frame(kind, payload=b""):
    return struct.pack(">BH", kind, len(payload)) + payload


def noise(secs, dbfs, seed):
    x = np.random.default_rng(seed).standard_normal(int(RATE * secs))
    return np.clip(x * 32768 * 10 ** (dbfs / 20), -32767, 32767)


def silence(secs):
    return np.zeros(int(RATE * secs))


# (label, start sample, end sample) of each noise burst in the test signal
PARTS = [silence(1), noise(4, -20, 1), silence(1), noise(4, -10, 2), silence(1)]
SIGNAL = np.concatenate(PARTS).astype(np.int16)
BURSTS = [("-20 dBFS", RATE * 1, RATE * 5), ("-10 dBFS", RATE * 6, RATE * 10)]


def db(p):
    return 10 * np.log10(max(p, 1e-12))


def rms_dbfs(x):
    return db(np.mean((x / 32768.0) ** 2))


def analyse(tx, rx):
    tx = tx.astype(float)
    rx = rx.astype(float)[:len(tx)]
    tx = tx[:len(rx)]
    print(f"\nnoise floor (first second, after 0.3 s): {rms_dbfs(rx[2400:RATE]):.1f} dBFS")
    # bulk delay: cross-correlate the whole recording with the whole signal
    n = 1 << int(np.ceil(np.log2(2 * len(tx))))
    xc = np.fft.irfft(np.fft.rfft(rx, n) * np.conj(np.fft.rfft(tx, n)), n)[:RATE]
    bulk = int(np.argmax(np.abs(xc)))
    print(f"bulk delay (cross-correlation peak): {bulk} samples = {bulk / 8:.1f} ms")
    start = max(0, bulk - 64)  # fit the impulse response from just before the peak
    for label, a, b in BURSTS:
        a2 = a + start + TAPS  # skip the onset so every tap sees signal
        # least-squares FIR: rx[k] ~ sum h[j] * tx[k - start - j]
        X = np.stack([tx[a2 - start - j:b - start - j] for j in range(TAPS)], axis=1)
        y = rx[a2:b]
        h, *_ = np.linalg.lstsq(X, y, rcond=None)
        res = y - X @ h
        order = np.argsort(-h ** 2)
        e = np.cumsum(h[order] ** 2) / np.sum(h ** 2)
        top = order[:int(np.searchsorted(e, 0.99)) + 1]
        spread = int(top.max() - top.min()) + 1  # first to last tap of the 99 %
        peak = start + int(np.argmax(np.abs(h)))
        erl = rms_dbfs(tx[a2 - bulk:b - bulk]) - rms_dbfs(y)
        erle = db(np.mean(y ** 2)) - db(np.mean(res ** 2))
        print(f"{label}: sent {rms_dbfs(tx[a:b]):.1f} dBFS, echo {rms_dbfs(y):.1f} dBFS"
              f" -> ERL {erl:.1f} dB; peak at {peak / 8:.1f} ms;"
              f" 99 % of the energy within {spread / 8:.1f} ms;"
              f" best linear ERLE {erle:.1f} dB (residual {rms_dbfs(res):.1f} dBFS)")


async def send(writer):
    loop = asyncio.get_running_loop()
    t = loop.time()
    data = SIGNAL.tobytes()
    for i in range(0, len(data), FRAME):
        writer.write(frame(0x10, data[i:i + FRAME]))
        await writer.drain()
        t += 0.02
        await asyncio.sleep(max(0, t - loop.time()))


async def call(reader, writer):
    rx = bytearray()
    cid = "?"
    sender = None
    try:
        while True:
            kind, n = struct.unpack(">BH", await reader.readexactly(3))
            data = await reader.readexactly(n)
            if kind == 0x01:
                cid = str(uuid.UUID(bytes=data))
                print(f"call {cid}: playing {len(SIGNAL) / RATE:.0f} s of test signal, stay silent")
                sender = asyncio.create_task(send(writer))
            elif kind == 0x10:
                rx += data
                if sender and sender.done() and len(rx) >= SIGNAL.nbytes:
                    break
            elif kind in (0x00, 0xFF):
                print("  call ended early")
                break
    except (asyncio.IncompleteReadError, ConnectionError):
        print("  connection closed")
    finally:
        if sender:
            sender.cancel()
        writer.close()
    if len(rx) < SIGNAL.nbytes:
        print(f"  only {len(rx) // 2 / RATE:.1f} s recorded; need {len(SIGNAL) / RATE:.0f} s")
        return
    for name, pcm in (("tx", SIGNAL.tobytes()), ("rx", bytes(rx))):
        with wave.open(f"echo-{cid}-{name}.wav", "wb") as w:
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(RATE)
            w.writeframes(pcm)
    analyse(SIGNAL, np.frombuffer(bytes(rx), dtype="<i2"))


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", default="0.0.0.0:9092", help="host:port")
    host, port = ap.parse_args().listen.rsplit(":", 1)
    srv = await asyncio.start_server(call, host, int(port))
    print(f"echo measurement listening on {host}:{port}; dial 800 and stay silent")
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
