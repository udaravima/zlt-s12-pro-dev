# 05 — Expansion findings: making the modem a real peer

Investigated 2026-09-26 after the user asked: can we upgrade the modem, give
the pair a dedicated network link, use partitions efficiently, and stop a
capable Linux computer from "running like a dumb USB modem"?

## What the modem actually is (measured, not assumed)

| Property | Value | Consequence |
|---|---|---|
| CPU | 4× ARM Cortex-A7 (v7l, NEON, 30 BogoMIPS counter is a WCN timer artifact) | Decent for small services |
| RAM | **39 MB total, ~7 MB free**, 16 MB zram swap (5.7 MB used) | **The real ceiling.** OOM killer is active and kills daemons under memory pressure (observed: sshd killed during scp receive) |
| Userland | glibc **2.27**, full sysvinit, sshd, python2.7, perl, sqlite3 | Cross-compile target: `armv7-a` glibc ≥2.27 (or static musl) |
| Rootfs | read-only ubifs | Can't install into / — but see below |
| `/mnt/userdata` | persistent, **rw, nosuid, nodev — no `noexec`** | **Arbitrary binaries can be stored AND executed from userdata** — root + exec-capable persistent storage means we can run anything without reflashing |
| Boot chain | `linuxboot.flash.locked=1`, verifiedbootstate=green, swupdate OTA images must be signed with vendor key (`/etc/swupdate_key/public.pem`) | Custom firmware would need the vendor's private key → **reflashing is effectively blocked** (and brick-risky: recovery partition expects signed .swu too) |

## Dedicated management link — WORKS (live-tested)

Brought up on the fly, no reboot, using spare NCM channel `usb2`
(192.168.9.1/30 router ↔ 192.168.9.2/30 modem). Ping 3–10 ms RTT.

Why usb2 is safe: netifd's `wwan` (proto ncm, /dev/ttyUSB0) is the LTE
failover data path and will claim one NCM channel when up (typically usb0);
`usb0`/`usb3` have netifd `device` stanzas already; **usb2 is claimed by
nothing**. mwan3 `wanb` is disabled.

### Throughput over the NCM link (20/40 MB, verified)

| Path | Method | Speed | Integrity |
|---|---|---|---|
| router → modem | raw nc (python2 receiver on modem) | **5.9 MB/s** | clean (byte-counted) |
| modem → router | raw nc (python2 sender) | **18.8 MB/s** | clean |
| router → modem | adb push (USB adb transport, not NCM) | 4.0 MB/s | md5-verified |
| modem → router | adb pull | 1.1 MB/s | md5-verified |
| router → modem | **scp over direct NCM ssh** | ~3 MB/s | **BROKEN — see below** |

### The scp-over-NCM failure — diagnosed

scp to 192.168.9.2 dies mid-transfer ("Connection reset by peer") with a
truncated file (9 MB and 15 MB of 20 MB in two tries). Modem dmesg shows:

```
Out of memory: Kill process 8127 (sshd) score 83 or sacrifice child
Killed process 8127 (sshd)
```

The link is fine; the modem OOM-kills sshd when scp's receive path (TCP
buffers + ubifs dirty page cache) outruns 39 MB of RAM. Same sshd + same
dropbear client over the adb-forward path worked (slower path = gentler
pressure). Practical rules:

- **Bulk data → adb push** (md5-clean, 4 MB/s, no modem-side memory pressure)
  or raw nc with a byte-counting receiver.
- **Avoid scp/ssh for bulk** until we've either shrunk sshd's footprint or
  paced the sender (e.g. `pv`-style rate limit, or small `dd bs=`).
- Any daemon we run on the modem must be tiny (static C, not python) — see RAM
  ceiling above. python2.7 alone is a large fraction of free RAM.

## Can we "upgrade the modem"? Honest answer

- **Custom OS on the modem: no.** Signed-boot chain, no vendor private key,
  recovery also expects signed images. Attempting it risks bracking a device
  whose firmware we cannot rebuild or re-sign. Recommendation: don't.
- **But we don't need to.** We already have: root shell, persistent rw +
  executable storage, sshd, python2, and a fast direct network link. That's
  enough to run cross-compiled services (mini_httpd, rsync-lite, busybox
  full, dropbear, collectd exporters…) started from userdata.
- **Open problem — autostart on modem boot:** rootfs and `/etc/init.d` are
  read-only, so we can't drop an init script in the usual place. `/var` IS
  persistent userdata (mount shows `/dev/ubi0_userdata on /var`), and the
  modem boots far less often than the router. Options to investigate:
  1. Router rc.local launches modem services via adb after router boot
     (works today, no modem changes).
  2. Find a writable rc hook the modem's rcS sources (check
     `/etc/init.d/rcS`, `populate-volatile.sh`, `udc.sh`, `usbenumfirst`).
  3. inittab is ro — but if any respawnable entry points at a writable path,
     that's a slot. Not yet investigated.

> **Update 2026-09-26 (later):** brick-risk concern is retired — full NAND
> dump taken and verified (`06-fw-dump.md`). The autostart question remains
> open; router-side launch via rc.local is the fallback that works today.
> The reusable dump tooling now lives permanently on the modem at
> `/mnt/userdata/tools/modem_send.py` (persistent + executable).

## Storage partitioning — measured

UBI volumes sum to ~116 of 124 MB; ~8 MB unallocated is mostly bad-block
reserve. Touching the UBI layout gains little (we need MBs, userdata already
has 14.6 MB free) and adds brick risk near the boot chain. Decision: use
`userdata` only. Cleaning candidates on the modem: `/mnt/userdata/data/`
holds config only (496 KB used) — nothing to clean; the 128 MB is mostly
system/recovery/DSP images we cannot safely remove.

## Fast package-cache path (new capability unlocked)

Modem→router raw nc hits **18.8 MB/s**, but adb pull is only 1.1 MB/s. So the
fast way to *fetch from* the modem is a network service on the modem, e.g. a
static tiny httpd serving `/mnt/userdata/pkgs/` — router `wget
http://192.168.9.2/...` should land near the nc numbers. This makes the
modem a genuinely fast package/cache server, fixing the slow-restore problem
in the /opt design (04).

## How obtained

Modem: `cat /proc/cpuinfo`, `free`, `/lib/libc.so.6`, `dmesg`, `ps w`,
`cat /etc/init.d/update-recovery.sh`, `swupdate` presence, `mount`.
Router/modem link: `ip addr add … dev usb2` + `adb shell ifconfig usb2 …`,
`ping -I usb2`; benchmarks via `scripts/modem_bench.py` (python2,
byte-counting recv/sender) pushed to modem `/mnt/userdata/bench.py` (cleaned
up afterwards), `dd|nc`, and `scp` with md5sum comparison (failure captured).
