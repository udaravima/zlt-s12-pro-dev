#!/usr/bin/env python3
"""sip_bot.py - a complete SIP phone application in one file, standard
library only: the template for a program that logs in to the PBX as a SIP
account (instead of using AudioSocket, doc 11 §8) and makes or takes calls.

What it does, in SIP terms:
  REGISTER   log in as --user (digest auth, MD5 or SHA-256), refresh it
  listen     answer incoming calls: a beep, then "parrot" (your voice back
             1 s later); keypad presses (RFC 4733 events) are printed, and
             '#' hangs up. The caller's audio is saved as sip-<call>.wav
  call N     dial N (100 = the RJ11 phone), play --play FILE.wav (8 kHz
             mono 16-bit) or a tone, then hang up: a notifier
Audio is G.711 u-law (PCMU), 20 ms RTP packets, the codec every SIP device
supports. It runs on one LAN with no NAT: no STUN/ICE, no Record-Route.

    export SIP_PASSWORD=...        # from the router's pjsip_secrets.conf
    python3 sip_bot.py --server 192.168.1.254 --user 202 listen
    python3 sip_bot.py --server 192.168.1.254 --user 202 call 100 --play hello.wav

Make a WAV it can play:  ffmpeg -i in.mp3 -ac 1 -ar 8000 -sample_fmt s16 out.wav
"""
import argparse
import asyncio
import hashlib
import math
import os
import random
import re
import socket
import struct
import sys
import time
import wave

RATE, FRAME = 8000, 160          # samples per second, per 20 ms packet
PT_PCMU, PT_DTMF = 0, 101


# ---------------------------------------------------------------- G.711 u-law

def ulaw_encode(x):
    sign = 0x80 if x < 0 else 0
    x = min(abs(x), 32635) + 0x84
    exp = max(0, x.bit_length() - 8)
    return ~(sign | (exp << 4) | ((x >> (exp + 3)) & 0x0F)) & 0xFF


def ulaw_decode(u):
    u = ~u & 0xFF
    x = (((u & 0x0F) << 3) + 0x84) << ((u >> 4) & 7)
    return 0x84 - x if u & 0x80 else x - 0x84


ENC = bytes(ulaw_encode(x) for x in range(-32768, 32768))   # index x + 32768
DEC = [ulaw_decode(u) for u in range(256)]


def to_ulaw(pcm):
    return bytes(ENC[s + 32768] for s in struct.unpack(f'<{len(pcm) // 2}h', pcm))


def from_ulaw(data):
    return struct.pack(f'<{len(data)}h', *(DEC[b] for b in data))


def tone(freq, secs, amp=8000):
    return b''.join(struct.pack('<h', int(amp * math.sin(2 * math.pi * freq * i / RATE)))
                    for i in range(int(RATE * secs)))


# ---------------------------------------------------------------- SIP messages

def new_tag():
    return '%08x' % random.getrandbits(32)


def parse(data):
    """-> (first line, [(name, value)], body)"""
    head, _, body = data.decode(errors='replace').partition('\r\n\r\n')
    lines = head.split('\r\n')
    return lines[0], [tuple(x.strip() for x in ln.split(':', 1)) for ln in lines[1:] if ':' in ln], body


def hdr(headers, name, all_=False):
    vals = [v for k, v in headers if k.lower() == name.lower()]
    return vals if all_ else (vals[0] if vals else '')


def build(first, headers, body=''):
    out = [first] + [f'{k}: {v}' for k, v in headers]
    if body:
        out.append('Content-Type: application/sdp')
    out.append(f'Content-Length: {len(body.encode())}')
    return ('\r\n'.join(out) + '\r\n\r\n' + body).encode()


def uri_of(nameaddr):
    m = re.search(r'<([^>]+)>', nameaddr)
    return m.group(1) if m else nameaddr.split(';')[0]


def digest(challenges, method, uri, user, password):
    """Answer the first WWW/Proxy-Authenticate challenge we support."""
    for ch in challenges:
        p = dict(re.findall(r'(\w+)="?([^",]+)"?', ch))
        alg = p.get('algorithm', 'MD5').upper()
        hf = {'MD5': hashlib.md5, 'SHA-256': hashlib.sha256}.get(alg)
        if not hf:
            continue
        h = lambda s: hf(s.encode()).hexdigest()
        ha1, ha2 = h(f'{user}:{p["realm"]}:{password}'), h(f'{method}:{uri}')
        a = (f'Digest username="{user}", realm="{p["realm"]}", nonce="{p["nonce"]}", '
             f'uri="{uri}", algorithm={alg}')
        if 'auth' in p.get('qop', '').split(','):
            cn = new_tag()
            a += f', qop=auth, nc=00000001, cnonce="{cn}"'
            resp = h(f'{ha1}:{p["nonce"]}:00000001:{cn}:auth:{ha2}')
        else:
            resp = h(f'{ha1}:{p["nonce"]}:{ha2}')
        a += f', response="{resp}"'
        if 'opaque' in p:
            a += f', opaque="{p["opaque"]}"'
        return a
    raise RuntimeError('no supported auth challenge')


def sdp(ip, port):
    sid = int(time.time())
    return (f'v=0\r\no=- {sid} {sid} IN IP4 {ip}\r\ns=sip_bot\r\nc=IN IP4 {ip}\r\nt=0 0\r\n'
            f'm=audio {port} RTP/AVP {PT_PCMU} {PT_DTMF}\r\na=rtpmap:0 PCMU/8000\r\n'
            f'a=rtpmap:{PT_DTMF} telephone-event/8000\r\na=fmtp:{PT_DTMF} 0-16\r\n'
            f'a=ptime:20\r\na=sendrecv\r\n')


def remote_media(body):
    ip = re.search(r'c=IN IP4 (\S+)', body).group(1)
    port = int(re.search(r'm=audio (\d+)', body).group(1))
    return ip, port


# ---------------------------------------------------------------- RTP

class Media(asyncio.DatagramProtocol):
    """One call's audio: sends `out` (16-bit PCM) every 20 ms, silence when
    empty; collects what arrives into `rec`; reports keypad events."""

    def __init__(self, on_key):
        self.out, self.rec = bytearray(), bytearray()
        self.on_key, self.last_ev = on_key, None
        self.peer = self.transport = None

    def connection_made(self, t):
        self.transport = t

    def datagram_received(self, data, addr):
        if len(data) < 12:
            return
        pt, ts = data[1] & 0x7F, struct.unpack('>I', data[4:8])[0]
        payload = data[12 + 4 * (data[0] & 0x0F):]
        if pt == PT_PCMU:
            self.rec += from_ulaw(payload)
        elif pt == PT_DTMF and payload and ts != self.last_ev:
            self.last_ev = ts      # one event = many packets with the same timestamp
            self.on_key('0123456789*#ABCD'[payload[0]] if payload[0] < 16 else '?')

    async def run(self):
        loop, seq, ts, ssrc = asyncio.get_running_loop(), random.getrandbits(16), 0, random.getrandbits(32)
        t = loop.time()
        while True:
            chunk = bytes(self.out[:FRAME * 2]).ljust(FRAME * 2, b'\0')
            del self.out[:FRAME * 2]
            hdr_ = struct.pack('>BBHII', 0x80, PT_PCMU, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
            self.transport.sendto(hdr_ + to_ulaw(chunk), self.peer)
            seq, ts, t = seq + 1, ts + FRAME, t + 0.02
            await asyncio.sleep(max(0, t - loop.time()))


# ---------------------------------------------------------------- the user agent

class Bot(asyncio.DatagramProtocol):
    def __init__(self, a):
        self.a, self.server = a, (a.server, 5060)
        self.waiting = {}            # (call-id, cseq) -> Future for the final response
        self.call = None             # the one call in progress (dict)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(self.server)       # no packet sent: just picks our LAN address
        self.ip = s.getsockname()[0]
        s.close()

    # transport
    def connection_made(self, t):
        self.transport = t
        self.port = t.get_extra_info('sockname')[1]

    def via(self):
        return f'SIP/2.0/UDP {self.ip}:{self.port};branch=z9hG4bK{new_tag()}{new_tag()};rport'

    def contact(self):
        return f'<sip:{self.a.user}@{self.ip}:{self.port}>'

    def datagram_received(self, data, addr):
        first, h, body = parse(data)
        if first.startswith('SIP/2.0'):
            key = (hdr(h, 'Call-ID'), hdr(h, 'CSeq'))
            code = int(first.split()[1])
            if code >= 200 and key in self.waiting and not self.waiting[key].done():
                self.waiting[key].set_result((code, h, body))
            elif code < 200:
                print(f'  {first[8:]}')
        else:
            asyncio.ensure_future(self.on_request(first.split()[0], first, h, body, addr))

    def reply(self, h, code, text, addr, extra=(), body=''):
        keep = [(k, v) for k, v in h if k.lower() in ('via', 'from', 'to', 'call-id', 'cseq')]
        self.transport.sendto(build(f'SIP/2.0 {code} {text}', keep + list(extra), body), addr)

    async def request(self, method, uri, h, body='', auth=True):
        """Send a request (retransmit on UDP until answered), answer one auth challenge."""
        for attempt in (0, 1):
            via = self.via()
            msg = build(f'{method} {uri} SIP/2.0', [('Via', via), ('Max-Forwards', '70')] + h, body)
            key = (hdr(h, 'Call-ID'), hdr(h, 'CSeq'))
            fut = self.waiting[key] = asyncio.get_running_loop().create_future()
            for _ in range(8):
                self.transport.sendto(msg, self.server)
                try:
                    code, rh, rbody = await asyncio.wait_for(asyncio.shield(fut), 0.5 if method != 'INVITE' else 60)
                    break
                except asyncio.TimeoutError:
                    if method == 'INVITE':
                        raise
            else:
                raise RuntimeError(f'{method}: no answer from {self.server[0]}')
            del self.waiting[key]
            if method == 'INVITE' and code >= 300:
                self.ack(uri, h, rh, via)        # same transaction: same branch
            if code in (401, 407) and auth and attempt == 0:
                name = 'Authorization' if code == 401 else 'Proxy-Authorization'
                chal = hdr(rh, 'WWW-Authenticate' if code == 401 else 'Proxy-Authenticate', True)
                n, m = hdr(h, 'CSeq').split()
                h = [(k, v) for k, v in h if k != 'CSeq' and k != name] + \
                    [('CSeq', f'{int(n) + 1} {m}'), (name, digest(chal, method, uri, self.a.user, self.a.password))]
                continue
            return code, rh, rbody, h

    def ack(self, uri, h, resp_h, via=None):
        """ACK a final INVITE response: a 2xx gets a new transaction (via=None),
        an error response is ACKed inside the INVITE's own (its Via)."""
        n = hdr(h, 'CSeq').split()[0]
        hh = [(k, v) for k, v in h if k in ('From', 'Call-ID')] + \
             [('To', hdr(resp_h, 'To')), ('CSeq', f'{n} ACK'), ('Max-Forwards', '70')]
        self.transport.sendto(build(f'ACK {uri} SIP/2.0', [('Via', via or self.via())] + hh), self.server)

    # registration
    async def register(self):
        cid, n, uri = f'{new_tag()}@{self.ip}', 1, f'sip:{self.a.server}'
        aor = f'<sip:{self.a.user}@{self.a.server}>'
        tag = new_tag()
        while True:
            h = [('From', f'{aor};tag={tag}'), ('To', aor), ('Call-ID', cid), ('CSeq', f'{n} REGISTER'),
                 ('Contact', self.contact()), ('Expires', '300'), ('User-Agent', 'sip_bot')]
            code, *_, h = await self.request('REGISTER', uri, h)
            n = int(hdr(h, 'CSeq').split()[0]) + 1
            if code != 200:
                sys.exit(f'REGISTER failed: {code} (wrong password? device not on the LAN subnet?)')
            print(f'registered as {self.a.user} at {self.a.server} (contact {self.ip}:{self.port})')
            await asyncio.sleep(240)

    # media
    async def start_media(self, peer):
        loop = asyncio.get_running_loop()
        t, m = await loop.create_datagram_endpoint(lambda: Media(self.on_key), local_addr=(self.ip, 0))
        m.peer, m.task = peer, None
        if peer:
            m.task = asyncio.ensure_future(m.run())
        return t, m

    def on_key(self, k):
        print(f'  key {k}')
        if k == '#' and self.call:
            asyncio.ensure_future(self.hangup())

    async def finish(self, why):
        c, self.call = self.call, None
        if not c:
            return
        if c['media'].task:
            c['media'].task.cancel()
        c['rtp'].close()
        name = f'sip-{c["cid"].split("@")[0]}.wav'
        with wave.open(name, 'wb') as w:
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(RATE)
            w.writeframes(c['media'].rec)
        print(f'call ended ({why}); heard {len(c["media"].rec) / 2 / RATE:.1f} s, saved {name}')
        c['done'].set()

    async def hangup(self):
        c = self.call
        if not c:
            return
        c['cseq'] += 1
        h = [('From', c['local']), ('To', c['remote']), ('Call-ID', c['cid']), ('CSeq', f'{c["cseq"]} BYE')]
        await self.finish('we hung up')
        await self.request('BYE', c['target'], h, auth=False)

    # incoming requests
    async def on_request(self, method, first, h, body, addr):
        cid = hdr(h, 'Call-ID')
        if method == 'OPTIONS':                      # Asterisk checks we're alive
            self.reply(h, 200, 'OK', addr, [('Allow', 'INVITE, ACK, BYE, CANCEL, OPTIONS')])
        elif method == 'INVITE' and self.call and self.call['cid'] == cid:
            pass                                     # retransmission of the one we handle
        elif method == 'INVITE' and (self.call or self.a.mode != 'listen'):
            self.reply(h, 486, 'Busy Here', addr)
        elif method == 'INVITE':
            tag = new_tag()
            to = f'{hdr(h, "To")};tag={tag}'
            h2 = [(k, to if k.lower() == 'to' else v) for k, v in h]
            print(f'incoming call from {hdr(h, "From")}')
            self.reply(h2, 180, 'Ringing', addr, [('Contact', self.contact())])
            await asyncio.sleep(1)
            rtp, m = await self.start_media(remote_media(body))
            m.out += tone(880, 0.3) + bytes(int(RATE * 0.7) * 2)      # beep, then parrot delay
            self.call = dict(cid=cid, local=to, remote=hdr(h, 'From'), cseq=0, rtp=rtp, media=m,
                             target=uri_of(hdr(h, 'Contact')), done=asyncio.Event())
            ok = build('SIP/2.0 200 OK', [(k, v) for k, v in h2 if k.lower() in ('via', 'from', 'to', 'call-id', 'cseq')]
                       + [('Contact', self.contact())], sdp(self.ip, rtp.get_extra_info('sockname')[1]))
            self.call['ok'] = ok
            for _ in range(8):                       # until ACK (or the call is gone)
                self.transport.sendto(ok, addr)
                await asyncio.sleep(0.5)
                if not self.call or self.call.get('acked'):
                    break
            asyncio.ensure_future(self.parrot())
        elif method == 'ACK':
            if self.call and self.call['cid'] == cid:
                self.call['acked'] = True
        elif method == 'BYE':
            self.reply(h, 200, 'OK', addr)
            if self.call and self.call['cid'] == cid:
                await self.finish('they hung up')
        elif method == 'CANCEL':
            self.reply(h, 200, 'OK', addr)
        else:
            self.reply(h, 501, 'Not Implemented', addr)

    async def parrot(self):
        """Feed what we hear back into what we send: 1 s delay line."""
        c = self.call
        sent = 0
        while self.call is c and c:
            new = c['media'].rec[sent:]
            sent += len(new)
            c['media'].out += new
            await asyncio.sleep(0.02)

    # outgoing call
    async def dial(self, number):
        cid, tag = f'{new_tag()}@{self.ip}', new_tag()
        uri = f'sip:{number}@{self.a.server}'
        rtp, m = await self.start_media(None)
        local = f'<sip:{self.a.user}@{self.a.server}>;tag={tag}'
        h = [('From', local), ('To', f'<{uri}>'), ('Call-ID', cid), ('CSeq', '1 INVITE'),
             ('Contact', self.contact()), ('User-Agent', 'sip_bot')]
        print(f'calling {number}')
        code, rh, rbody, h = await self.request('INVITE', uri, h, sdp(self.ip, rtp.get_extra_info('sockname')[1]))
        if code != 200:
            rtp.close()
            print(f'call failed: {code}')
            return
        target = uri_of(hdr(rh, 'Contact')) or uri
        self.ack(target, h, rh)
        m.peer = remote_media(rbody)
        m.task = asyncio.ensure_future(m.run())
        self.call = dict(cid=cid, local=local, remote=hdr(rh, 'To'), cseq=int(hdr(h, 'CSeq').split()[0]),
                         rtp=rtp, media=m, target=target, done=asyncio.Event())
        print('answered')
        if self.a.play:
            with wave.open(self.a.play) as w:
                if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (RATE, 1, 2):
                    sys.exit(f'{self.a.play}: need 8000 Hz mono 16-bit')
                m.out += w.readframes(w.getnframes())
        else:
            for f in (523, 659, 784, 1047):          # a little chime
                m.out += tone(f, 0.25) + bytes(800)
        secs = len(m.out) / 2 / RATE + 1
        try:
            await asyncio.wait_for(self.call['done'].wait(), max(secs, self.a.seconds))
        except asyncio.TimeoutError:
            await self.hangup()


async def main():
    ap = argparse.ArgumentParser(description='SIP account bot for the home PBX (doc 12 §5.4)')
    ap.add_argument('--server', required=True, help="the PBX (the router's LAN address)")
    ap.add_argument('--user', required=True, help='SIP account, e.g. 202')
    ap.add_argument('--port', type=int, default=5070, help='local SIP port (default 5070)')
    sub = ap.add_subparsers(dest='mode', required=True)
    sub.add_parser('listen', help='answer calls: beep, then your voice back 1 s later')
    c = sub.add_parser('call', help='dial a number, play audio, hang up')
    c.add_argument('number')
    c.add_argument('--play', help='8 kHz mono 16-bit WAV to play when answered')
    c.add_argument('--seconds', type=float, default=0, help='stay on the line at least this long')
    a = ap.parse_args()
    a.password = os.environ.get('SIP_PASSWORD') or sys.exit('set SIP_PASSWORD')
    loop = asyncio.get_running_loop()
    bot = Bot(a)
    await loop.create_datagram_endpoint(lambda: bot, local_addr=(bot.ip, a.port))
    reg = asyncio.ensure_future(bot.register())
    await asyncio.sleep(0.5)
    if a.mode == 'call':
        await asyncio.sleep(0.5)
        await bot.dial(a.number)
    else:
        print('waiting for calls (Ctrl-C to quit)')
        await reg


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
