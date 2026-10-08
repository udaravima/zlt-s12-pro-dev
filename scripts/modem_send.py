#!/usr/bin/env python2
# Stream a file (or block device) from the modem to a TCP client.
# Usage: modem_send.py <path> [port]
# Client (router): nc 192.168.9.2 <port> > outfile
import socket, sys, time

path = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5530
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', port))
s.listen(1)
sys.stderr.write("READY sending %s on %d\n" % (path, port))
sys.stderr.flush()
c, a = s.accept()
t0 = time.time()
n = 0
with open(path, 'rb') as f:
    while True:
        buf = f.read(65536)
        if not buf:
            break
        c.sendall(buf)
        n += len(buf)
c.close()
s.close()
sys.stderr.write("SENT %s: %d bytes in %.1fs\n" % (path, n, time.time() - t0))
