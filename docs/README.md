# ZLT S12 Pro — router + modem: from "dumb USB modem" to a real peer

The project outgrew its original goal. Timeline of scope:

1. **Package storage** — the router's 16MB flash is 90% full; use a few MB of
   the modem's spare NAND (docs 01–04).
2. **Modem as a peer** — dedicated network link, fast transfers, efficient
   use of both boxes (doc 05).
3. **Full firmware dump** — complete NAND image as brick insurance and for
   offline analysis (doc 06).
4. **Voice/SMS/VoIP audit** — what the phone port, SIM path, and SLT VoIP
   VLAN can actually do (doc 07).
5. **WWAN data link** — the NCM dial path fixed and documented; internet
   from the Mobitel SIM verified, pppoe kept as the default route (doc 08).
6. **RJ11 phone port revived** — the Le9641 SLIC found on the router side,
   driven from OpenWrt with two small kernel modules + Microsemi's API in
   userspace: rings, dial tone, hook detection, audio clip playback (doc 09,
   code in `fxs/`).
7. **Home PBX** — Asterisk 23 on the router, LAN-only: the RJ11 phone is
   extension 100 (dial out with the keypad, rings with caller ID), LAN
   devices log in over SIP (201–203), and apps / AI agents take calls over
   AudioSocket (ext 800) (doc 11; hands-on guide doc 12).

## Current state: home PBX (2026-10-11, verified live)

- **Asterisk 23.1.0** from the OpenWrt telephony feed (apk), set up by
  `slic-asterisk-setup`; SIP on the LAN address only, random per-account
  passwords generated on the router (never in git). 16 MB resident RAM.
- **Verified on the real phone:** dial tone; keypad digits detected in
  software; **601** announcement, **600** echo test (39 s, 0 frames
  dropped), **800** to a demo app on a LAN PC (beep + parrot, recording
  saved); **calls to 100** ring with caller ID, answer gives two-way audio;
  a caller hanging up mid-ring stops the bell.
- **Softphone verified** (account 201 on a mobile over Wi-Fi): it rings the
  handset with caller ID, and the handset dials it (`201#`), both with
  two-way audio. Ringback/busy use Sri Lanka's tones (ITU E.180 list).
- **Not done:** echo cancellation (an app hears its own voice ~30 dB down);
  handset keypresses during a call reach apps only as tones; SLT SIP trunk
  deferred, `voip_br` stays disabled.

## Current state: RJ11 phone port (2026-10-08, verified live)

- **Installed as packages (`slic-fxs` + 2 kmods), service starts at boot**
  with a boot-loop failsafe; `slicctl daemon` gives dial tone on lift, rings
  on `echo "ring 20" > /var/run/slic.ctl`, plays clips with `slic-play`.
  End-to-end ring → answer → clip → dial tone verified, it **survives a
  reboot**, and **caller ID works** (Bellcore FSK, shown on a Uniden AS7412,
  2026-10-11). Buildable by other owners as an OpenWrt feed from this repo.
- **Working:** SLIC detected (`PCN 0x49`, Le9641
  rev 8); init and calibration with Tozed's own profiles; **phone rings**,
  **450 Hz dial tone heard**, hook on/off reported; **audio clips play** through
  the MT7621 PCM bus (loopback-verified clean 1 kHz).
- **Key facts:** SLIC on SPI0 CS1 (mode 0); PCM on GPIO 9–12; GPIO 32
  `slic_clk_switch`: 0 = router PCM, 1 = modem PCM; the MT7621 audio PLL must
  be programmed or no clock reaches the SLIC.
- **Not done:** confirming the V.23/DTMF caller-ID variants on a phone.
  (DTMF dialling and SIP/Asterisk: done 2026-10-11, see the PBX state
  above and doc 11.) Not upstreamable to OpenWrt as is
  (Microsemi licence; doc 09 §13).

## Current state (2026-10-02, all items verified live)

**Working now:**
- Root shell on modem via adb; sshd on modem with router-key auth via
  `adb forward` (direct-NCM ssh works for shells but OOM-fails on bulk).
- Dedicated management link on spare NCM channel `usb2`:
  192.168.9.1/30 (router) ↔ 192.168.9.2/30 (modem), 3–10 ms RTT.
  Raw throughput: **5.9 MB/s router→modem, 18.8 MB/s modem→router**.
  Live-tested only — *not yet persisted across reboots*.
- Full 128 MB modem NAND dump on host (`modem-fw-dump/`),
  md5-verified.
- Modem-side tooling installed persistent + executable at
  `/mnt/userdata/tools/` (modem_send.py).
- **Backup internet via the modem, as a hybrid** (doc 08 + its Oct 2 update):
  ModemManager runs permanently (LuCI cellular page, SIM state, registered
  Mobitel EDGE, signal 70%) while `wwan` (proto ncm) dials on `/dev/ttyUSB3`
  — a port MM doesn't need. usb0 gets CGNAT 100.64.2.104/28 from the
  modem and passes traffic (174–600 ms, EDGE). No MM/ncm port collision.
- **Metric-based failover, demonstrated once**: pppoe default at metric 10
  (slt_internet_wan), usb0 default at metric 50; pppoe link down → usb0
  default takes over. mwan3 is **not installed** (its config file is an
  inert leftover; failover needs no mwan3). Caveat: after a pppoe restart
  the usb0 default route once vanished (restored by bouncing wwan; root
  cause unknown — re-verify routes after any pppoe restart).
- Keepalive for the modem link: hotplug script + cron ping the usb0
  gateway every minute (hardcoded IP — fragile across lease changes).

**4G verdict (Oct 2 test, final):** with the modem forced LTE-only, the only
visible network is **Dialog LTE (PLMN 413-11)** — Mobitel LTE is not visible
at this location at all. The Mobitel SIM (413-01) is refused registration
on Dialog LTE (CEREG status 8, cause 15 — same national-roaming denial as
the Sep 26 "emergency-only" scare). WCDMA: nothing here. GSM: registers
home instantly. **No band lock exists** (restored tz.profile has
`SPLBAND=0,0,0,0,0,0`; no frequency lock) — the blocker is coverage + SIM,
not config. 4G at this location requires a **Dialog SIM** or antenna
repositioning toward Mobitel LTE coverage.

**Decisions locked in:**
- **Never reflash the modem** — signed boot chain (`flash.locked=1`), vendor
  key unavailable, recovery expects signed images. Instead: run cross-compiled
  armv7 binaries from `/mnt/userdata` (persistent, rw, exec-capable).
- **Bulk data path**: `adb push` (router→modem, 4 MB/s, md5-clean) or raw NCM
  stream. Never ssh/scp for bulk — modem's 39 MB RAM gets it OOM-killed
  (observed twice).
- **Storage design**: Entware-style `/opt` in router tmpfs + tarball on modem
  userdata, apk `--root` managed (doc 04), with restore over the fast NCM
  fetch path (doc 05) instead of slow adb pull. Kernel modules can't live in
  /opt — they stay on the router overlay.
- **pppoe (SLT fibre) is the primary uplink** (metric 10); modem NCM link is
  backup only (metric 50). Failover is plain route-metric, no mwan3.
- **Hybrid modem management**: ModemManager stays running for LuCI/SIM/SMS;
  the ncm proto owns ttyUSB3 for the data dial. All-MM data is dead (MM
  builds a PPP bearer; this firmware has no PPP server — LCP timeout).
- **tz.profile persistence is fragile**: the file was silently restored to a
  pristine vendor copy between Oct 1–2, wiping the Sep 26 SPTESTMODEM edit.
  Mechanism unknown — don't rely on tz.profile edits surviving.

- Voice/SMS control paths verified live (doc 07): AT works via
  `sms_tool` on `/dev/ttyUSB0` (ModemManager must be stopped for raw AT on
  MM's ports — it grabs them and wedges in `failed` state); oFono on the modem
  exposes `VoiceCallManager` (dial/answer via dbus-send) but **no SMS atom** —
  SMS goes through AT. **Mobitel SIM: NOT simlocked, NOT network-locked, NOT
  band-locked** — registers on Mobitel GSM as home. Runtime RAT preference is
  "GSM only" (dbus flip, reverts on modem reboot; tz.profile says LTE-only —
  which wins at boot is unverified since the vendor restore).
  RJ11 "Phone/Line" exists but no SLIC chip is visible in software on either
  board (likely wired to the modem module's headset-audio pair — unverified
  until a live call). SLT VoIP VLAN 10 gives no DHCP offer → dead end without
  SLT provisioning.

**Next steps (priority order):**
1. **Understand the usb0 route-vanish** (doc 08 update): reproduce cleanly
   with `ifdown/ifup slt_internet_wan` (NOT `ip link down` — that once took
   the house offline), find why wwan_4 loses its default route, then make
   the keepalive gateway-agnostic. Until then, check
   `ip route show default` after every pppoe restart.
2. 4G direction decision: Dialog SIM test, or antenna repositioning for
   Mobitel LTE (the Oct 2 test closed the config path — it's coverage/SIM).
3. Voice/SMS exercise with the Mobitel SIM: SMS round-trip, call test on
   the RJ11 handset — see doc 07 next steps.
4. Router hygiene: remove luci-theme-bootstrap (1.5 MB = 45% of overlay),
   delete `/etc/config/*.bak-*`, resolve `.apk-new` files, clean duplicate
   `wwan` entries in the firewall zone.
5. Persist the usb2 management link (uci stanza on router; modem side via
   adb at boot).
6. Cross-compile toolchain (armv7 glibc ≥2.27, or static musl) → tiny static
   httpd/busybox for the modem serving `/mnt/userdata/pkgs/`; then the /opt
   scheme with fast NCM restore.

## Documents

| File | Contents |
|------|----------|
| `01-router.md` | Router inventory: hardware, flash layout, overlay pressure |
| `02-modem.md` | Modem inventory: NAND/UBI layout, userland, USB gadget constraints |
| `03-interconnect.md` | Access chain (adb/ssh), benchmarks, what persists where |
| `04-design-options.md` | Package-storage designs: dead ends + recommended /opt scheme |
| `05-expansion.md` | Modem as peer: RAM/boot ceilings, NCM link, throughput, OOM diagnosis |
| `06-fw-dump.md` | Full NAND dump: method, verification, pitfalls |
| `07-voice-sms.md` | Voice/SMS/VoIP: AT channel, oFono, SIM state, RJ11 wiring, SLT VLAN probe |
| `08-wwan-ncm-data.md` | WWAN data link: ncm proto architecture, SPTZCMD dial mechanism, no-dial bug, routing metrics |
| `09-phone-port-fxs.md` | RJ11 FXS port: Le9641 SLIC on the MT7621, firmware archaeology, kernel modules, clock switch, PCM bring-up, caller ID, how to reproduce |
| `10-phone-port-architecture.md` | Phone port architecture + onboarding: components, interfaces, runtime flows, state machine, timing, design decisions, failure modes, labs, extension points |
| `11-pbx-asterisk.md` | Home PBX: Asterisk on the router (apk), LAN-only SIP, the phone as extension 100 over AudioSocket, keypad DTMF detection, app/AI-agent hook (ext 800), pitfalls, SLT trunk plan |
| `12-pbx-setup-guide.md` | PBX hands-on guide: install, everyday commands, every setting and where it lives, adding accounts and your own numbers, connecting softphones / desk phones / apps, test checklist, troubleshooting, backup, how it was set up here |
| `debrief.md` | From-the-beginning briefing: status, stories, decisions, open items |

## Artifacts

- `docs/` — this knowledge base
- `scripts/` — `modem_send.py` (modem-side streamer),
  `router_dump_mtd.sh` (dump orchestrator), `modem_bench.py` (throughput)
- `fxs/` — RJ11 phone port: kernel modules (`kmod-slic-mt7621/`),
  `slic-fxs/` package (daemon, init script, config, dial hook), `asterisk/`
  (PBX templates + `slic-asterisk-setup`), `slicctl` SLIC tool, `build.sh`, profile extractor, PCM analyser, test audio,
  decompiled stock device tree. Microsemi API and Tozed profile data are
  fetched/generated locally, not committed (see doc 09 §11)
- `modem-fw-dump/` — partition images mtd0–7 + `reference-md5s.txt`
  (+ `mtd7-ubipac-2nd.bin`, the verification duplicate — safe to delete)

All facts gathered live on 2026-09-26 via `/usr/bin/ssh
root@router.lan` and `adb`; each doc records its commands in a
"How obtained" section. Update this README's *Current state* whenever facts
change — the numbered docs are historical records with update notes, not
living documents.
