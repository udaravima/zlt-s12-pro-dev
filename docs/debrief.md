# ZLT S12 Pro — Debrief

*Written 2026-09-26 end-of-day, updated 2026-10-02. A from-the-beginning
briefing for someone (including a future session) who needs the whole picture
fast. Pointers go to the numbered docs; this file tells the story.*

## The mission, and how it grew

The box is a TOZED ZLT S12 PRO router with a built-in UNISOC SL8563 LTE
modem. When we started, the modem was a dumb USB peripheral and the
router was full. Every expansion since then followed from one constraint:
**16 MB router flash, 90% full, and a modem with spare NAND, RAM, and a
root shell that nobody was using.**

Scope grew in five phases, each documented in its own doc:

1. **Inventory** — what both boxes actually are (docs 01–02).
2. **Package storage design** — how to use the modem's spare space
   (docs 03–05).
3. **Firmware dump** — brick insurance (doc 06).
4. **Voice/SMS audit** — what the SIM and the RJ11 port can do (doc 07).
5. **WWAN data link** — internet from the modem, done properly (doc 08).

## The two machines, in one paragraph each

**Router**: MT7621, OpenWrt 25.12.5 (apk, no opkg), 16 MB flash with a
3.6 MB overlay that's 90% full, 121 MB tmpfs, 248 MB RAM. Primary WAN is
PPPoE over SLT fibre (VLAN 20). Runs adbd at boot so we can reach the
modem. Busybox is old: nc can't listen, no timeout command.

**Modem**: quad-core ARM Cortex-A7 (armv7l), kernel 4.4, 39 MB RAM + zram
(OOM killer active — bulk ssh/scp into it dies), 16 MB rootfs **read-only**,
persistent `/mnt/userdata` (rw + exec). SSH as root on :22, adb on :5555.
Boot chain is signed and locked → **we never reflash it**; instead we run
cross-compiled armv7 binaries from `/mnt/userdata`.

**Interconnect** (doc 03/05): adb push 4 MB/s, raw NCM stream 5.9/18.8
MB/s via a management link we built on spare NCM channel `usb2`
(192.168.9.1/30 ↔ 192.168.9.2/30). Rule learned twice: never ssh/scp
bulk into the modem — it OOM-kills sshd.

## What actually works now (all verified live)

- **Root access to both boxes**: router ssh; modem via adb and ssh
  (key-based, persistent), plus the direct NCM management link (not yet
  persisted across reboots).
- **Full 128 MB NAND dump** of the modem on the host, md5-verified
  twice over (doc 06).
- **Modem is carrier-unlocked**: Mobitel SIM tested — NOT simlocked, NOT
  network-locked, NOT band-locked. The Sep 26 "emergency-only" scare was a
  persisted "LTE only" preference meeting Dialog-only LTE coverage (doc 07).
- **4G question closed (Oct 2)**: forced LTE-only, the only visible network
  is **Dialog LTE (413-11)**; the Mobitel SIM is refused registration on it
  (cause 15 — national roaming denied). Mobitel LTE is not visible here at
  all; no band mask exists (restored tz.profile `SPLBAND=0,0,0,0,0,0`).
  4G here = Dialog SIM or antenna repositioning. Nothing left to configure.
- **Backup internet via the modem, as a hybrid** (doc 08 + Oct 2 update):
  ModemManager runs permanently (LuCI/SIM state, EDGE registered) while
  `wwan` (proto ncm) dials on `/dev/ttyUSB3` — no port collision. usb0 gets
  CGNAT 100.64.2.104/28 and passes traffic (174–600 ms, EDGE).
  **pppoe keeps the default route** at metric 10; usb0 sits at metric 50.
  Failover is plain route-metrics (mwan3 turned out to be **not
  installed** — its config file is an inert leftover); demonstrated live
  once, with a caveat: after a pppoe restart the usb0 default route
  vanished once (bouncing wwan restored it; cause unknown — see open items).
- **Voice readiness** (doc 07): oFono exposes VoiceCallManager (dial/answer
  via dbus), the audio chain up to the CP is wired (CSFB mode), SMS must
  go via AT (`sms_tool`) because oFono has no SMS atom. Untested until we
  place a live call — no SIM in during the audio test window.

## The debugging stories worth retelling

**1. Why the modem looked locked when it wasn't (doc 07).** A LTE-only
preference, persisted in the modem's own boot file (`tz.profile`
`AT+SPTESTMODEM=3,254`), met a spot where the only visible LTE network
was Dialog's — the SIM was Mobitel. National roaming denied →
emergency-only. Switching to auto / GSM made it register home instantly.
The `mode` lives on the modem; writes go via oFono/dbus because the CP
refuses SPTESTMODEM from any other AT channel.

**2. Why `wwan` said "up" with no internet for days (doc 08 §3).** The
router's `ncm` proto script had a latent JSON bug: our leftover
`option mode 'auto'` in uci triggered a lookup of the spreadtrum `modes`
map — which doesn't exist — and the script's `json_select ..` then
popped its cursor to the JSON root, where `connect` reads empty. The dial
command was **silently never sent**. Fix: delete the `mode` option.
The dial itself turns out to be wild: base64-encoded shell command to the
modem (`AT+SPTZCMD` → `connmanctl ndisdial 'AT^NDISDUP="usb0",1,1'`) —
the router asks the modem to dial itself, and the modem runs that string
as root.

**3. The outage I caused (doc 08 §4–5).** When the dial finally worked,
the modem's DHCP installed a default route at metric 0 — same as pppoe —
and displaced the pppoe default. House internet + my own session died.
The operator restored it manually; the durable fix is `wwan.metric=50`
(set **before** any `ifup`; the dynamic DHCP interface inherits it).
Lesson recorded: never bring up a new uplink with metric 0.

**4. The 4G question, answered twice over (Oct 2).** Sep 26's theory was
"maybe a band mask hides Mobitel LTE" (tz.profile had `SPLBAND=1,0,128,0,0,0`,
unreadable live). Oct 2 killed the theory twice: the vendor-restored
tz.profile has **no mask** (`0,0,0,0,0,0`), and a forced LTE-only scan sees
exactly one network — Dialog LTE (413-11) — which the Mobitel SIM is
forbidden to register on (cause 15). Mobitel LTE simply isn't broadcast
here. The blocker was never config; it's coverage + which SIM you hold.

**5. The route that vanished (Oct 2, unresolved).** After a pppoe restart,
the usb0 default route quietly disappeared: wwan_4 (the dynamic DHCP
interface) kept its IP but netifd's route array for it went empty, so
failover was dead while everything *looked* up. Bouncing wwan restored it.
Root cause unknown — until it's understood, `ip route show default` must be
checked after every pppoe restart. (Same session lesson: test failover with
`ifdown/ifup`, never `ip link down` — the latter once took the house
offline and produced the 8–10 s pings that made the outage look worse
than EDGE.)

## Decisions locked in

- **Never reflash the modem** — signed boot chain, no vendor key.
- **Bulk data path**: adb push or raw NCM; never ssh/scp into the modem.
- **Package storage**: Entware-style `/opt` in tmpfs + tarball on modem
  userdata, apk `--root` managed; restore over fast NCM (doc 04/05).
- **pppoe is the primary uplink** (metric 10); modem is backup (metric 50).
  Failover = plain route metrics; **no mwan3** (package not installed).
- **Hybrid modem management** (Oct 2): ModemManager runs for LuCI/SIM/SMS;
  the ncm proto owns ttyUSB3 for the data dial. All-MM data is dead — MM
  builds a PPP bearer and this firmware has no PPP server (LCP timeout).
- **tz.profile persistence is fragile**: the file was silently restored to
  a vendor copy between Oct 1–2, wiping the Sep 26 SPTESTMODEM edit.
  Mechanism unknown; don't trust tz.profile edits to survive.
- Docs: README = living status; numbered docs = historical records.

## Persisted changes made to the devices (cumulative)

Router (`/etc/config/network`): `wwan` = proto ncm, device `/dev/ttyUSB3`,
apn mobitel, pdptype IP, metric 50, delay 5, ifname usb0 (plus inert
`multipath='backup'` leftover); `wwan.mode` deleted (doc 08 §3 bug);
`slt_internet_wan.metric='10'`. Router (other): mwan3 `wwan` section
deleted (config file inert — package not installed);
`/etc/hotplug.d/iface/99-wwan-ping` keepalive script; crontab entry pinging
the usb0 gateway every minute (hardcoded IP — fragile). Modem
(`/mnt/userdata`): `tools/modem_send.py`; tz.profile Sep 26 edit was
**wiped by the silent vendor restore** (file now `SPTESTMODEM=3,254`,
`SPLBAND=0,0,0,0,0,0`). Runtime-only (resets on reboot): TechnologyPreference
"GSM only" (dbus flip), ModemManager running, usb2 link needs manual setup.
Untouched by design: modem rootfs, ril.ini, firewall.

## Open items, in priority order

1. **usb0 route-vanish** (Oct 2 regression): after a pppoe restart, wwan_4's
   default route disappears (IP stays, netifd route array empties).
   Reproduce cleanly with `ifdown/ifup slt_internet_wan`, find the cause,
   then make the keepalive gateway-agnostic (it hardcodes the DHCP
   gateway). Until then: check `ip route show default` after every pppoe
   restart.
2. **4G direction**: Dialog SIM test or antenna repositioning — the Oct 2
   test proved config can't fix it (coverage + SIM).
3. **Voice/SMS exercise**: SMS round-trip via `sms_tool`, live call on
   the RJ11 handset.
4. **Router hygiene**: luci-theme-bootstrap (1.5 MB = 45% of overlay),
   `*.bak-*` files, `.apk-new` orphans, duplicate `wwan` entries in the
   firewall zone.
5. **Persist the usb2 management link** (uci on router + adb-side modem
   setup at boot).
6. **Cross-toolchain** (armv7, static musl or glibc ≥ 2.27) → tiny static
   httpd on the modem → `/opt` scheme with fast NCM restore.
7. **tz.profile restore mechanism** — unknown what restores it; matters
   for any future persisted-RAT strategy.

## Dead ends (don't revisit)

USB mass storage (gadget lacks f_mass_storage), NFS/9p/CIFS/sshfs (no
kernel support / no sftp-server), ext4 loop images (no ext4 in kernel),
SLT VoIP VLAN 10 (no DHCP offer — needs SLT provisioning), SMS via oFono
(no MessageManager atom), SPTESTMODEM writes from the router AT channel
(CP-refused; dbus only), **all-ModemManager data** (MM builds a PPP
bearer; firmware has no PPP server — LCP timeout after 30 s), **band-mask
hunting** (Oct 2: no mask exists; the LTE blocker is coverage + SIM),
**Mobitel 4G at this location** (LTE-only scan sees Dialog 413-11 only;
Mobitel SIM roaming-denied there — needs Dialog SIM or antenna).

## Artifacts

- `docs/` — this knowledge base (01–08 + README + this file)
- `scripts/` — modem_send.py, router_dump_mtd.sh, modem_bench.py
- `modem-fw-dump/` — mtd0–7 images + md5s (+ ofonod.bin strings
  copy, tz.profile backup)
