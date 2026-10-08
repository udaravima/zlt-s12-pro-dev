#!/usr/bin/env python2
# Throughput benchmark for the router<->modem NCM link.
# Usage on modem (via adb): modem_bench.py <recv|send> [port]
# recv = modem receives (tests router->modem); send = modem sends (tests modem->router)
import socket, sys, time

mode = sys.argv[1] if len(sys.argv) > 1 else 'recv'
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5523
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', port))
s.listen(1)
sys.stderr.write("READY %s on %d\n" % (mode, port))
sys.stderr.flush()
c, a = s.accept()
t0 = time.time()
if mode == 'recv':
    n = 0
    while True:
        d = c.recv(65536)
        if not d:
            break
        n += len(d)
else:
    buf = 'x' * 65536
    target = 40 * 1024 * 1024
    n = 0
    while n < target:
        k = c.send(buf)
        n += k
dt = time.time() - t0
print("%s %.1f MB in %.2fs = %.2f MB/s" % (mode.upper(), n / 1e6, dt, n / 1e6 / dt))
c.close()
s.close()
