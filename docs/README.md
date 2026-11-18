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
8. **SMS gateway + logs that survive a reboot** — smstools3 receives and
   sends SMS through the modem; received SMS, the system log, Asterisk's log
   and call history moved from RAM to the USB root filesystem (doc 13).
9. **Firewall check-up + provisioning** — a read-only audit script and an
   idempotent provisioning script that restores the hardened firewall,
   SSH and nginx state after a reset (doc 14).
10. **Rebuild after a factory reset, scripted** — extroot attach, the
   package list, the PBX setup with ARI, then the firewall: one runbook
   (doc 15).
11. **Phone pages in the web interface** — LuCI Services → Phone: a status
   page for the line, the PBX, phones and calls, SIP account management,
   and a form for the line's settings (doc 16).
12. **Modem serial ports mapped** — what each of the modem's five ports is
   (three AT channels, two binary streams), matched from both ends, and one
   job per AT channel (doc 17).

## Current state: modem ports (2026-11-18)

- **Port map** (doc 17):
  - `ttyUSB0`, `ttyUSB3` and `ttyUSB4` are three separate AT channels into
    the modem;
  - `ttyUSB1`/`ttyUSB2` are Unisoc's binary diagnostics and log streams,
    the "garbage" ports.
  - Matched from the router's USB interfaces and, over adb, from the
    modem's gadget config, atrouter and engpc.
- **New assignment, applied and tested:**
  - the LTE dial on `ttyUSB3` (`delay 5`);
  - smsd on `ttyUSB4`, with registration reports switched off;
  - `ttyUSB0` left for people and tools.
  - `scripts/router_modem_ports.sh` checks it (`status`) and sets it
    (`apply`).
- **The trap it exposed:**
  - An AT port nobody has open collects the modem's `+CREG` reports. The
    first dial on the new port read one as the modem's maker and stopped
    ("Unsupported modem").
  - Recovered by hand. Switching the reports off fixed it for good: the
    setting is modem-wide.
  - A re-dial while smsd runs then came up in 12 s.
- **Also found, not changed:** LTE isn't a backup route right now
  (`defaultroute 0`, mwan3 not running), so a fibre outage takes the house
  offline (doc 17 §8).

## Current state: rebuild runbook + package cleanup (2026-11-17)

- **LuCI pages (`luci-app-slic-fxs`, doc 16):**
  - **Status:** the line, the daemon, Asterisk, the SIP phones with where
    they registered from, active calls, HEP, and recent problems, with a
    warning when PJSIP's module table refuses a module.
  - **Accounts:** the SIP logins: show a password on request; add, re-key or
    remove one without restarting Asterisk. Uses `slic-accounts` (new in
    `slic-fxs` r7), which `slic-asterisk-setup` now uses too.
  - **Phone line:** a form over `/etc/config/slic`.
  - **Tested on the router:** the backends, the Save & Apply path, the
    account operations (on a throwaway extension), and every button offline.
    **Not yet looked at in a browser.**
  - **An outage I caused while testing (about 4 minutes):** rpcd's umask 077
    made the regenerated accounts file unreadable to Asterisk, and
    `#tryinclude` skipped it silently, so every SIP account vanished. Fixed
    (explicit `chmod`) and retested. 202, the TCP phone, lost its
    registration; it logged back in by itself and was seen again at 19:44
    (doc 16 §4.2).
  - **A bug found on the way:** procd compares environments without the last
    4 bytes of each entry, so settings changes like dial timeout 3 → 4
    never restarted the daemon. `slic-fxs` r6 now restarts it explicitly on
    every reload (doc 16 §4.1).
- **Live SIP view (hobby phase 0):** HEP is on. Asterisk copies every SIP
  message to `sngrep -L udp:0.0.0.0:9060` on the PC (`scripts/router_hep.sh`,
  doc 12 §7.1). The catch: OpenWrt's pjproject has only 32 PJSIP module
  slots, all already in use. HEP showed `Running` while capturing nothing
  until `res_pjsip_history` gave up its slot. **No slots are spare now.**
  Verified across an Asterisk restart, and on the PC with softphones 201
  and 204 registering (both now have their new passwords).
- **`scripts/router_extroot.sh`**: status, and attach (never formats; an old
  overlay on the stick is archived, not deleted). **`scripts/router_packages.sh`**:
  snapshot, check and restore of the 126 packages added on top of the
  firmware (`scripts/router-packages.txt`), plus 3 locally built ones. Both
  were tested live and on stand-in folders. The full reset sequence hasn't
  been run yet (doc 15 §3).
- **The firmware already has extroot support** (`block-mount`, usb-storage,
  ext4). Internal writable flash: 4.1 MiB. Extroot: `LABEL=extroot`, 7.2 GiB.
- **Removed 13 unused Asterisk packages** (phoneprov, `app-sms`, 6 file
  formats, …). Asterisk went from 73 to 60 packages; ARI still answers and a
  test call played (doc 15 §4).
- **`slic-asterisk-setup` (r6)** now installs the ARI group when `ari.conf`
  enables ARI. Before, a reset would have brought the PBX back without the
  call agent's ARI. Installed on the router 2026-11-17: r6 (the reload
  fix, doc 16 §4.1), then r7 (`slic-accounts`). Earlier files are in
  `/root/backups/slic-fxs-r5-20261117-160223.tgz` and
  `slic-fxs-r6-20261117-194339.tgz`.
- **Found while checking the docs (fixed 2026-11-18, see above):** since the 2026-11-05
  rebuild, smsd uses `ttyUSB4`, the same modem port as the wwan data link.
  It works only because wwan dials at boot before smsd starts. smsd's
  "Unexpected input: +CREG" lines also made up 20 % of the persistent
  system log. Doc 13 §2.5 has the details and the proposed fix.

## Current state: firewall check-up (2026-11-15)

- **Audit (live): 0 FAIL, 8 WARN.** Nothing is exposed. WAN input and forward
  are `REJECT`, there are no port forwards, and UPnP and AMI are off.
  45,216 WAN packets were rejected in about 12 h. The WARNs are services
  where the firewall is the only layer: SSH on every interface with
  passwords allowed, and the ARI nginx site open to any source. There are
  also the stock IPsec passthrough rules, and duplicate `wwan` entries.
- **Found:** stock OpenWrt's board script for this model stages "add `wwan`
  to zone wan" on *every* boot. The next LuCI Save & Apply commits it as
  one more duplicate (doc 14 §3).
- **`scripts/router_fw_provision.sh`:** dry run live (9 changes planned),
  `--apply` tested on copies only. **Not applied live yet**: it switches SSH
  to key-only, which needs the owner's OK.

## Current state: PBX rebuilt after a factory reset (2026-11-05)

- The router was reset to factory settings and moved to **OpenWrt 25.12.6**;
  kmods r3 and `slic-fxs` r5 were rebuilt for kernel 6.12.112 (build script
  in 432e850). Asterisk then exited at every start: restoring the LuCI
  backup before the install had created `/etc/asterisk` as `0700`.
- **Rebuilt (doc 12 §8.1):** folder mode, the missing Asterisk packages,
  accounts 201–205 (new passwords), the `/srv/log/asterisk` folders, the ARI
  app's `Stasis()` routes. Files the backup skipped are now listed in
  `/etc/sysupgrade.conf`. `slic-fxs` r6 and later (installed 2026-11-17) make the
  setup repair the folder and log paths itself.
- **Verified:** Asterisk up, modules running (incl. `UnicastRTP`), an
  internal call to 601 played through, local-time call record. **Not yet
  re-tested:** handset, softphones, the ARI app.

## Current state: SMS + persistent logs (2026-10-13, verified live)

- **SMS:** smstools3, `/etc/smsd.conf`; received SMS in
  `/root/spool/sms/incoming/`, send by dropping a file in `outgoing/`. Receive
  and send both verified. The modem's own SMS memory has zero slots; messages
  live on the SIM (`"SM"`), which smsd empties.
- **Logs on disk:** system log `/srv/log/messages` (1 MiB + one `.old`),
  Asterisk log and call history (CDR, local time) in `/srv/log/asterisk/`.
  New packages: `asterisk-cdr-csv`, `zoneinfo-asia`.
- **Reboot test passed** (after the 2026-10-14 commit). **Not done:**
  Asterisk's own log doesn't rotate. Rebuild steps and traps: doc 13.

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
| `12-pbx-setup-guide.md` | PBX hands-on guide: install, everyday commands, every setting and where it lives, adding accounts and your own numbers, connecting softphones / desk phones / apps, test checklist, troubleshooting, watching SIP live (HEP → sngrep), backup, how it was set up here |
| `13-sms-and-persistent-logs.md` | SMS gateway (smstools3: SIM storage, init strings, send/receive) and moving logs, call history and SMS off RAM: what must stay in `/var`, system log to disk, Asterisk log dir + CDR + time zone; rebuild guide |
| `14-firewall-security.md` | Firewall security check-up: who can reach the router (CGNAT, carrier network, LAN), every check and today's result, the boot-time duplicate `wwan` bug, the audit and provisioning scripts (what they change, safety rails, test mode) |
| `15-rebuild-after-reset.md` | Factory reset → working router: why the order is extroot → backup → packages → own builds → PBX → firewall, the runbook with commands, what each script was tested with, the package list and the 2026-11-17 Asterisk cleanup |
| `16-luci-phone-pages.md` | LuCI Services → Phone: what the status page, the SIP accounts page and the line form show, where each value comes from, cost per refresh, security, the procd bug that kept settings from reaching the daemon, developing and testing |
| `17-modem-serial-ports.md` | The modem's five serial ports: what each one is (matched from both ends), who uses which and why, the trap where an unread AT port poisons the next dial, recovering a dial that gave up, the check/apply script, and the finding that LTE isn't a backup route |
| `debrief.md` | From-the-beginning briefing: status, stories, decisions, open items |

## Artifacts

- `docs/` — this knowledge base
- `scripts/` — `modem_send.py` (modem-side streamer),
  `router_dump_mtd.sh` (dump orchestrator), `modem_bench.py` (throughput),
  `router_fw_audit.sh` / `router_fw_provision.sh` (firewall check-up and
  provisioning, doc 14), `router_extroot.sh`, `router_packages.sh` +
  `router-packages.txt` (rebuild after a reset, doc 15), `router_hep.sh`
  (SIP capture to sngrep, doc 12 §7.1)
- `fxs/` — RJ11 phone port: kernel modules (`kmod-slic-mt7621/`),
  `slic-fxs/` package (daemon, init script, config, dial hook),
  `luci-app-slic-fxs/` (LuCI pages, doc 16), `asterisk/`
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
