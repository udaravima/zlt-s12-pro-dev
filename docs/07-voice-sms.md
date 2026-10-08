# 07 — Voice, SMS, VoIP: what the hardware can actually do

Investigated live on 2026-09-26. Everything below was verified on the boxes.

> **Update 2026-09-26 (SIM test):** a **Mobitel** SIM was inserted and tested
> live — see the "SIM test — lock check" section at the end. Verdict: **not
> SIM-locked, not network-locked, not band-locked (GSM)**. The modem is now
> registered on Mobitel GSM. The original no-SIM sections below are kept as
> the historical record.

> **Update 2026-10-02 (4G test + tz.profile restore):** two things below are
> now outdated. (1) **tz.profile was silently restored to a pristine vendor
> copy** between Oct 1 and Oct 2 (mtime Jul 9): the Sep 26 edit
> (`SPTESTMODEM=6,254`, auto) is gone, the file again says `AT+SPTESTMODEM=3,254`
> (LTE-only) — and the `AT+SPLBAND=1,0,128,0,0,0` band-mask caveat is **moot**:
> the restored file has `AT+SPLBAND=0,0,0,0,0,0` (factory, no mask). The
> restore mechanism is unknown; tz.profile edits are therefore **fragile**.
> (2) A controlled LTE-only test (runtime dbus flip, no reboot) settled the
> LTE question: with the modem forced LTE-only, the scan sees exactly one
> network — `+COPS: (1,"","","41311",7)` — i.e. **Dialog LTE** (413-11 is
> Dialog's LTE PLMN, per the Sep 26 finding), **not Mobitel**. The Mobitel
> SIM (IMSI 41301…) is refused registration on it: CEREG status 8,
> `+SPERROR: 1,15` (cause 15 — no suitable cells / roaming denied), COPS? = 0.
> WCDMA-only registers to nothing at this location. GSM-only registers home
> (41301) instantly at 70% signal. **Verdict: Mobitel LTE is not even
> visible here — 4G on this SIM at this location is impossible; 4G requires
> a Dialog SIM or antenna repositioning toward Mobitel LTE coverage.** The
> band-mask theory is disproven (no mask, and the network is visible anyway —
> it's registration that's denied). Runtime state left: TechnologyPreference
> "GSM only" (dbus, reverts on modem reboot).

## TL;DR

| Capability | Status | Why |
|-----------|--------|-----|
| SMS | **ready, blocked on SIM** | `sms_tool` + working AT port (`/dev/ttyUSB0`) |
| Cellular calls (dial/hangup/answer) | **ready, blocked on SIM** | oFono `VoiceCallManager` on modem + AT commands |
| Call audio on RJ11 handset | **unverified wiring** → *2026-10-08: SLIC found on the router; rings, tone, hook, clip playback work (doc 09)* | audio chain exists; no SLIC chip found — see RJ11 section |
| VoIP over SLT VLAN 10 | **dead end for now** | VLAN exists, but no DHCP offer; no SIP client anywhere |

## AT command channel (the control path)

- Router `/dev/ttyUSB0-4` via `option` driver (USB interfaces
  1.8/1.9/1.10/1.11/1.13 of `1782:4055`).
- **Only ttyUSB0 speaks AT reliably** (tested: `AT` → `OK`). ttyUSB1/2 give
  no response (diag/log channels). ModemManager lists ttyUSB3/4 as
  AT-capable secondary ports.
- Bridge architecture, confirmed from atrouter's fd table:
  router ttyUSB ↔ modem ttyGS2/3/4 ↔ `atrouter` ↔ `/dev/stty_lte4/5/6/20/25`
  ↔ CP (via ptmx/pts pairs). `atrouter` (`/usr/bin/atrouter`, 220 MB VSZ)
  also holds 4 abstract unix sockets.
- **ModemManager is installed and runs at boot** (fibocom plugin; recognizes
  "Spreadtrum Communication CO. V1.0.1-B7", 9 ports). It grabs ttyUSB0/3/4,
  then its init fails: "Couldn't check unlock status: Serial command timed
  out" → modem stuck in `failed` state. **Stop it before raw AT work:**
  `/etc/init.d/modemmanager stop`. (After a stop/start cycle it did not
  re-detect the modem — needs a hotplug event.)
- `sms-tool` package: binary is **`sms_tool`** (underscore; the dash name
  does not exist). Subcommands: `send/recv/delete/status/ussd/at`, options
  `-d device` `-D debug`. Works on ttyUSB0 once ModemManager is stopped.
  Router busybox has **no `timeout` command** — use sms_tool's own timeouts
  or `kill`.

## SIM state (2026-09-26)

- **No SIM inserted** (owner confirmed). `AT+CPIN?` → `+CME ERROR: 10`
  (SIM not inserted). `AT+CEREG?` → `2,8` (registered emergency-only, LTE).
  `AT+CSQ` → `5,99`.
- Stale history in `/mnt/userdata/data/ril.ini`: earlier registration on
  **Dialog (41311)** with PDP IP 10.0.0.180 and Dialog DNS — from a
  previous SIM era; not current state.
- Router `wwan` interface has `apn 'hutch3g'` — **wrong for Dialog**; set
  the correct Dialog APN before using LTE failover once a SIM is in.
- oFono `SimManager`: `Present=false`. ModemManager's unlock-check timeout
  (above) is consistent with a SIM-less modem.

## oFono on the modem (D-Bus control)

- `ofonod -nd -p rildev,ril,rilmodem,connman`; system `dbus-daemon` running;
  `dbus-send`/`dbus-monitor` exist on the modem; python2 has **no dbus
  module** (script D-Bus on modem = shell + dbus-send only).
- `/ril_0`: `Powered=true`, `Online=true`. Interfaces exposed:
  `RadioSettings`, `SimManager`, `CallVolume`, `VoiceCallManager`.
  **No MessageManager (SMS) atom, no NetworkRegistration interface** →
  SMS must go through AT (`sms_tool`), not oFono. Calls can use either.
- Dialing once a SIM exists, either way:
  ```
  # oFono (on modem):
  dbus-send --system --print-reply --dest=org.ofono /ril_0 \
    org.ofono.VoiceCallManager.Dial string:"0771234567" string:""
  # AT (on router, ModemManager stopped):
  sms_tool -d /dev/ttyUSB0 at "ATD0771234567;"
  sms_tool -d /dev/ttyUSB0 at "ATH"        # hang up
  sms_tool -d /dev/ttyUSB0 at "ATA"        # answer incoming
  sms_tool -d /dev/ttyUSB0 at "AT+CLCC"    # list current calls
  ```
- `RadioSettings`: TechnologyPreference "LTE only"; available GSM/WCDMA/CDMA.
  Kernel cmdline `ltemode=lcsfb` → **CS fallback**: circuit-switched calls
  drop to 2G/3G (Dialog has legacy networks). Emergency numbers list is
  populated (08, 112, 911 …).
- oFono runtime config lives in `/mnt/userdata/data/ofono/`
  (radiosetting: TechnologyPreference=11, autoLoadApnMode=1).

## Call audio path (modem module)

Four ALSA cards on the modem:

| Card | Purpose |
|------|---------|
| `sprdphone` (card0) | PMIC sc2721 codec; normal + voice (VBC) paths |
| `alli2s` (card1) | external I2S (`i2s_bt_sco0` DAI) — NXP TFA smart amp |
| `saudiolte` (card2) | AP↔CP PCM for LTE/CS voice |
| `saudiovoip` (card3) | AP↔CP PCM for VoIP audio |

- `audiocmp` daemon — note `/etc/init.d/S90audiocmp` is an **ELF binary,
  not a script** (SPRD's audio HAL, ~22 MB). From its strings: routes
  earpiece/speaker/main-mic/back-mic during calls; VBC control channel to
  CP over `/dev/spipe_lte6` (contains "found CP_CSFB"); I2S owner switching
  via `/proc/pin_switch/iis0_sys_sel/{ap,cp1}_iis0`; headset detection via
  `/sys/class/switch/h2w/state`; in-call volume/gain/mute control; bundles
  its own amixer/pcm (tinyalsa-style) implementation.
- Device tree: **no SLIC node**. I2C devices on the modem are smartphone
  parts: `bma2x2` accelerometer, `ltr_558als` ambient light,
  `adaptive-touchscreen`, `ocp8137` flash LED, `sc2700_chg`/`fan54015_chg`
  battery chargers, `sensor-sub`/`sensor-main`. The "sl8563-cpe-2h10"
  module is a smartphone-derived board.
- Router side: lsusb shows only the Spreadtrum modem — no USB audio; only
  SPI device is the NOR flash. **The router SoC has no audio at all.**
  *(Update 2026-10-08: wrong. The MT7621 has a PCM controller at
  `0x1E002000`; OpenWrt just has no driver or DT node for it, and the SLIC on
  SPI CS1 isn't declared in OpenWrt's DT. See doc 09.)*

## RJ11 "Phone/Line" port — open wiring question

> **Update 2026-10-08: resolved, see [09-phone-port-fxs.md](09-phone-port-fxs.md).**
> Neither hypothesis below holds. The port is a real FXS line driven by a
> Microchip **Le9641 SLIC on the router's MT7621** (SPI0 CS1 for control,
> MT7621 PCM on GPIO 9–12 for audio), which is why no SLIC showed up on the
> modem. GPIO 32 (`slic_clk_switch`) moves the SLIC's audio bus between the
> router (0, SIP mode) and the modem (1, VoLTE mode). On OpenWrt it now rings,
> gives dial tone, detects hook state and plays audio clips.

The port exists (owner-confirmed, labeled Phone/Line). But no SLIC (FXS)
chip is visible in software on either board, and the router has no audio
hardware. Two hypotheses:

- **A: wired to the modem module's headset audio pair** (mic/speaker + h2w
  detect). Cheap "phone" interface; consequence: a real analog phone likely
  will **not ring** (no ring generator), but off-hook/pressing keys may
  register as a headset and call audio may flow.
- **B: hidden analog aux path** — audiocmp references `ad23` /
  `sprd-codec-ad1-voice` capture channels and an analog codec at
  `0x40000000`, which could be an undocumented line interface.

Verdict requires a live call test (see next steps). A physical look at the
PCB around the RJ11 would also settle it.

## VoIP over SLT VLAN 10 — dead end for now

- `VOIP_Net` bridge = lan1 + wan.10; the stock config had `voip_br` with
  **proto dhcp** — the stock router itself was the SIP endpoint (a TOZED
  VoIP client in their firmware, gone since the vanilla OpenWrt flash).
- Live probe 2026-09-26 (temporarily enabled `slt_voip_wan` dhcp on
  wan.10, then reverted): udhcpc broadcast 4 discovers over ~12 s,
  **no offer** → SLT is not serving the VoIP VLAN on this line (stock TOZED
  also shipped it disabled).
- No SIP stack exists on the modem either: no SIP strings in `atrouter`,
  nothing listening on 5060.
- If ever wanted: needs SLT-side provisioning + SIP credentials, plus a
  SIP client; RJ11 audio integration would run through the modem's ALSA
  cards (saudiovoip/sprdphone) — a cross-compiled baresip/pjsua on
  `/mnt/userdata` is the plausible route.

## LEDs

`blue:phone`, `blue:modem`, `red:modem`, `blue:signal1-5` exist on the
router; all currently have trigger `[none]` (unwired in this firmware).
They can be driven via sysfs once call-state events exist.

## How obtained

All via `/usr/bin/ssh root@router.lan` and `adb shell` on
2026-09-26: `sms_tool` AT probes, `dbus-send` queries of org.ofono,
`lsusb`, DT walk (`/sys/firmware/devicetree/base`), I2C/SPI device dumps,
`atrouter` fd inspection, binary strings of S90audiocmp, ModemManager
logread traces, and the DHCP probe of wan.10.

## Next steps

1. ~~Insert a SIM → verify `AT+CPIN?` = READY~~ — done 2026-09-26 (Mobitel,
   see SIM test section; registered on GSM, `AT+CNUM` returns nothing — no
   MSISDN stored on the SIM).
2. Set a Mobitel APN in `CGDCONT` and router `wwan` (both currently blank /
   stale `hutch3g`) before using packet data. Confirm the exact APN string
   from Mobitel's own docs when enabling data.
3. SMS round-trip: `sms_tool send/recv/status` + a `ussd` test.
4. Call test: handset on RJ11, `ATD…;` from a second phone, check audio
   both ways, `/sys/class/switch/h2w/state` on off-hook, and whether an
   incoming call rings the handset.
5. Decide ModemManager's fate (disable at boot, or keep and use mmcli if
   it initializes once a SIM is present).
6. RAT preference persistence: after a modem reboot the TechnologyPreference
   reverts to "LTE only" (`persist.radio.modem.workmode = 3,254` in
   `/mnt/userdata/data/ril.ini`), which leaves a Mobitel SIM in
   emergency-only here. Either change that value (ril.ini is persistent
   userdata) or set GSM mode at boot via the autostart mechanism (see 05).
7. Optional: wire `blue:phone` LED to call state; oFono dial/answer script
   on the modem (dbus-send is enough).

## SIM test — lock check (Mobitel SIM, 2026-09-26)

> This section supersedes the no-SIM "SIM state" facts above.

A Mobitel SIM (ICCID `-`, IMSI `-`) was
inserted. Question asked: is the modem SIM-locked, network-locked, or
band-locked? **Answer: none of those.** What looked like a lock was a
mode setting plus RF:

- **Not SIM-locked**: `AT+CPIN?` → `+CPIN: READY`. All `AT+CLCK`
  facilities queried 0: `SC` (SIM), `PN` (network), `PU` (network subset),
  `PP` (service provider), `PC` (corporate). (`PF` returns 255 = query
  unsupported, not a lock.)
- **Not network-locked (carrier whitelist)**: with RAT preference switched
  to "GSM only" the modem **registered on Mobitel as its HOME network**
  (`+CREG: 1` / `+COPS: 0,0,"Mobitel"` / `+SPNWNAME: 413,01,"Mobitel"`),
  and a GSM scan sees all three national operators: Mobitel `41301`,
  Hutch `41308`, Dialog `41302`. A Dialog-branded lock would have refused
  registration.
- **Not band-locked on GSM**: registration + scan above prove the GSM 900
  layer is open. LTE caveat below.
- **The confusing part was "LTE only"**: oFono RadioSettings had
  TechnologyPreference **"LTE only"** (persisted as
  `persist.radio.modem.workmode = 3,254` in
  `/mnt/userdata/data/ril.ini`). In LTE-only mode the scan sees exactly
  one PLMN: `41311` (Dialog LTE — Dialog broadcasts a separate LTE PLMN
  41311; its 2G/3G PLMN is 41302, confirmed by the GSM scan). The signal
  there is weak (`+CSQ: 6` ≈ −104 dBm), and a Mobitel SIM on Dialog =
  national-roaming denied → `CEREG 2,8` emergency-only. That *looked* like
  a lock but wasn't.
- **RF reality at this location**: GSM 900 is strong (`+CSQ: 21` ≈ −67 dBm
  on Mobitel). **No WCDMA detected** ("WCDMA only" scan: zero networks,
  `+CSQ: 0`). **No Mobitel LTE visible** (LTE-only scan with the Mobitel
  SIM found only Dialog's 41311). Undetermined whether missing Mobitel LTE
  is pure coverage/antenna placement or an LTE band-mask in NV — the AT
  set has no band-query command (SPBAND/CBAND/^BAND all unsupported). GSM
  disproves a general band lock; LTE bands remain unverified.
- **Packet data**: `AT+CGATT?` → 1 (GPRS attached); the network even
  activated a PDP context with blank CGDCONT APNs (contexts 1–11 all
  APN-less; context 11 = IMS). No MSISDN on SIM (`AT+CNUM` empty).
- **Live state left behind**: registered on Mobitel GSM (auto COPS),
  PS-attached, TechnologyPreference = "GSM only" (set via oFono dbus).
  This does **not** survive a modem reboot — see next steps item 6.
  ModemManager was stopped for the AT probes and left stopped.

Commands used (ModemManager stopped first):

```
sms_tool -d /dev/ttyUSB0 at "AT+CPIN?"          # +CPIN: READY
sms_tool -d /dev/ttyUSB0 at "AT+CLCK=\"SC\",2"  # +CLCK: 0  (same for PN/PU/PP/PC)
sms_tool -d /dev/ttyUSB0 at "AT+CIMI"           # -
sms_tool -d /dev/ttyUSB0 at "AT+CCID"           # ICCID
# raw capture needed for COPS=? scans (sms_tool times out first):
cat /dev/ttyUSB0 > /tmp/cops.out & printf "AT+COPS=?\r" > /dev/ttyUSB0; sleep 150
# RAT switching (on modem, via adb):
dbus-send --system --print-reply --dest=org.ofono /ril_0 \
  org.ofono.RadioSettings.SetProperty string:"TechnologyPreference" \
  variant:string:"GSM only"     # options seen: "LTE only" (default), "GSM only",
                                # "WCDMA only", "CDMA/EvDo auto"
```

## Network mode: auto — set, persisted, verified (2026-09-26, later session)

> Supersedes the "does not survive a modem reboot" caveat above: the
> RAT preference is now **"LTE/GSM/WCDMA auto"** and survives reboots.

How it was done and what was learned:

- **The mode enum**: `AT+SPTESTMODEM=<mode>,254` on this CP:
  `3` = LTE only, `6` = LTE/GSM/WCDMA auto, `11` = WCDMA only,
  `15` = GSM only. Direct **writes** to SPTESTMODEM from the router's
  ttyUSB0 AT channel are refused (`+CME ERROR: 3`) — the CP only
  accepts them from the RIL's own channel. So changes go through oFono:
  ```
  adb shell 'dbus-send --system --print-reply --dest=org.ofono /ril_0 \
    org.ofono.RadioSettings.SetProperty string:"TechnologyPreference" \
    variant:string:"LTE/GSM/WCDMA auto"'
  ```
  The dbus `AvailableTechnologies` list is misleading (didn't show the
  auto option); the full supported-string table was found by pulling
  `/usr/sbin/ofonod` and running `strings` — full list: GSM only,
  WCDMA only, CDMA only, EvDo only, GSM/WCDMA/CDMA/EvDo auto,
  LTE/CDMA/EvDo auto, **LTE/GSM/WCDMA auto**,
  LTE/CDMA/EvDo/GSM/WCDMA auto, LTE only, LTE/WCDMA auto.
- **Persistence mechanism**: `/mnt/userdata/data/tz.profile` (odd name,
  ~200 bytes of AT commands) is **replayed by `atrouter` at CP boot**
  (binary references `/mnt/data/tz.profile`). It contained
  `AT+SPTESTMODEM=3,254` (the LTE-only lock), plus `AT+SPLBAND=1,0,128,0,0,0`
  (see caveat below). Changed the line to `AT+SPTESTMODEM=6,254`.
  `persist.radio.modem.workmode` in `ril.ini` is a RIL-*written* mirror
  (it flipped to `6` by itself after the dbus change); no need to edit it.
- **Backup**: `/mnt/userdata/tz.profile.bak-20260926` on the modem and
  host copy `tzprofile-backup-20260926.txt`.
- **Verification**: rebooted the whole modem with `AT+CFUN=1,1` (clean —
  adb/ssh drop for ~2 min, USB re-enumerates, adbd auto-reconnects;
  router's own internet unaffected). After reboot:
  TechnologyPreference = "LTE/GSM/WCDMA auto", `AT+SPTESTMODEM?` →
  `6,254`, `AT+COPS?` → registered "Mobitel" GSM (home). In auto mode
  the modem scans both PLMNs, rejects Dialog LTE (roaming denied) and
  camps on Mobitel GSM — it will use Mobitel LTE automatically wherever
  it is receivable.
- **Caveat — possible LTE band restriction**: `tz.profile` also sets
  `AT+SPLBAND=1,0,128,0,0,0` at every boot. `AT+SPLBAND?` read is
  refused (`+CME ERROR: 4`), so whether this masks LTE bands (and
  explains why no Mobitel LTE is visible here) is unverified. If a
  Mobitel-LTE location is ever tested and LTE still doesn't register,
  clear/change that line first.
- **Live state left behind**: modem in auto mode, registered on Mobitel
  GSM, `tz.profile` patched. The brief "WCDMA only" test during mapping
  dropped registration but was immediately followed by the auto switch.
  ModemManager on the router remains stopped.
