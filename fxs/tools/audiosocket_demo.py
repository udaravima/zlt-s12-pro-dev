#!/usr/bin/env python3
"""Minimal AudioSocket app: the template for plugging an app or AI agent
into the PBX (extension 800 hands its call to AUDIOSOCKET_APP).

Asterisk connects to us over TCP, one connection per call, and both sides
exchange frames:  kind (1 byte) | length (2 bytes, big endian) | payload
  0x01  call UUID (16 bytes), always first
  0x10  audio: 16-bit signed little-endian mono at 8 kHz (20 ms = 320 bytes)
  0x03  a DTMF key (1 ASCII byte), Asterisk -> app only; sending one back
        makes Asterisk drop the call
  0x00  hangup        0xFF  error
Asterisk hangs up if it hears nothing from the app for 2 s, so an app must
keep sending audio (silence counts) for as long as the call should last.

This demo is a parrot: a beep, then everything the caller says comes back
1 s later. Receiving and sending are separate: the sender runs on its own
20 ms clock and plays silence when it has nothing to say, which keeps the
call alive while the caller is quiet (or, for a real agent, while the model
thinks). An agent replaces the delay line with "speech-to-text -> model ->
text-to-speech" feeding `out`. Each call's incoming audio is also saved as
audiosocket-<uuid>.wav.

    python3 audiosocket_demo.py [--listen 0.0.0.0:9092]
"""
import argparse
import asyncio
import math
import struct
import uuid
import wave

RATE = 8000
DELAY = 1.0  # seconds before the caller hears themselves


def tone(freq, secs, amp=8000):
    n = int(RATE * secs)
    return b"".join(struct.pack("<h", int(amp * math.sin(2 * math.pi * freq * i / RATE)))
                    for i in range(n))


def frame(kind, payload=b""):
    return struct.pack(">BH", kind, len(payload)) + payload


FRAME = 320  # bytes per 20 ms


async def speak(writer, out):
    """Send one 20 ms frame every 20 ms: from `out`, or silence."""
    loop = asyncio.get_running_loop()
    t = loop.time()
    while True:
        chunk = bytes(out[:FRAME])
        del out[:FRAME]
        writer.write(frame(0x10, chunk.ljust(FRAME, b"\0")))
        await writer.drain()
        t += 0.02
        await asyncio.sleep(max(0, t - loop.time()))


async def call(reader, writer):
    peer = writer.get_extra_info("peername")
    # the beep and silence that fill the delay line before the caller's voice
    out = bytearray(tone(880, 0.3))
    out += bytes(int(RATE * DELAY) * 2 - len(out))
    rec = sender = None
    try:
        while True:
            kind, n = struct.unpack(">BH", await reader.readexactly(3))
            data = await reader.readexactly(n)
            if kind == 0x01:
                cid = str(uuid.UUID(bytes=data))
                print(f"call {cid} from {peer[0]}")
                rec = wave.open(f"audiosocket-{cid}.wav", "wb")
                rec.setnchannels(1), rec.setsampwidth(2), rec.setframerate(RATE)
                sender = asyncio.create_task(speak(writer, out))
            elif kind == 0x10:
                if rec:
                    rec.writeframes(data)
                out += data
            elif kind == 0x03:
                print(f"  DTMF {data.decode(errors='replace')}")
            elif kind == 0x00:
                print("  hangup")
                break
            elif kind == 0xFF:
                print(f"  error from Asterisk: {data.hex()}")
                break
    except (asyncio.IncompleteReadError, ConnectionError):
        print("  connection closed")
    finally:
        if sender:
            sender.cancel()
        if rec:
            print(f"  saved {rec.getnframes() / RATE:.1f} s to audiosocket-{cid}.wav")
            rec.close()
        writer.close()


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", default="0.0.0.0:9092", help="host:port")
    host, port = ap.parse_args().listen.rsplit(":", 1)
    srv = await asyncio.start_server(call, host, int(port))
    print(f"AudioSocket demo listening on {host}:{port}")
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
