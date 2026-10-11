# 10 — Phone port: architecture and onboarding guide

Written 2026-10-11. Companion to [09-phone-port-fxs.md](09-phone-port-fxs.md):
doc 09 is the **story** (how the hardware was worked out, every experiment);
this doc is the **map** (what the parts are, how they talk, why they're built
this way, how to change them). Code: [`fxs/`](../fxs). The Asterisk PBX
built on top (the phone as extension 100, LAN SIP devices, apps over
AudioSocket) is [11-pbx-asterisk.md](11-pbx-asterisk.md).

*Updated 2026-10-11 for the Asterisk link: keypad dialling, `call`/`hangup`
commands, the AudioSocket bridge, non-blocking audio device.*

## How to read this

| You are | Read | Time |
|---|---|---|
| **New developer / intern** | §1 big picture → §2 components → §4 runtime flows → §9 hands-on labs → §8 when something breaks | ~1 hour, plus the labs |
| **Architect / reviewer** | §1 → §3 interfaces → §5 state machine → §6 timing → §7 design decisions → §10 extension points | ~30 min |
| **Someone adding an application** (SIP, intercom, notifier, AI agent) | §3 interfaces → §10 extension points → doc 11 §8 | ~15 min |

Terms are explained the first time they appear and collected in §11.

---

## 1. The big picture

A normal analog phone plugs into the router's RJ11 jack. The phone is "dumb":
the **line** has to power it, ring it, notice when it's picked up, and carry
audio. On this router a dedicated chip does all of that: the **SLIC**
(Subscriber Line Interface Circuit), a Microchip **Le9641**. Our software's
job is to drive that chip from OpenWrt.

```
 ┌────────────── PC (developer) ──────────────┐
 │ OpenWrt SDK → fxs/build.sh → 3 .apk files  │
 └────────────────────┬───────────────────────┘
                      │ scp + apk add
 ┌────────────────────▼──────────── Router: MT7621, OpenWrt 25.12.5 ─────────────────────┐
 │                                                                                         │
 │  userspace   /etc/init.d/slic (procd) ──starts──► slicctl daemon ◄── /var/run/slic.ctl  │
 │                                                     │   │    │        (text commands) │
 │                                       Microsemi VP-API-II   │    └──► /var/run/slic.state
 │                                       (vp886, in-process)   │                            │
 │                                                     │       │ caller-ID / clip audio     │
 │  ─────────────────────────────────────────────────── │ ───── │ ────────────────────────── │
 │  kernel       spidev ◄── slic_spi_cs1.ko        /dev/spidev0.1   /dev/slicpcm            │
 │               spi-mt7621                            │       │ mt7621_pcm_raw.ko          │
 │  ─────────────────────────────────────────────────── │ ───── │ ────────────────────────── │
 │  hardware     SPI0 (CS0 = boot flash, CS1 = SLIC)   │       │ PCM controller + audio PLL │
 │                                                     ▼       ▼   GPIO 9-12, GPIO 32 switch │
 └─────────────────────────────────────────── Le9641 SLIC ── RJ11 ── phone ────────────────┘
```

Two separate wires between the router and the SLIC, two separate jobs:

| Path | Bus | Carries | Our software |
|---|---|---|---|
| **Control** | SPI (serial, 1 MHz, mode 0) | register reads/writes: "ring now", "is the phone off-hook?", calibration | `slic_spi_cs1.ko` → `/dev/spidev0.1` → VP-API-II inside `slicctl` |
| **Audio** | PCM (2.048 MHz bit clock, 8 kHz frames) | voice samples, 16-bit, 8000 per second, each direction | `mt7621_pcm_raw.ko` → `/dev/slicpcm` |

**The one fact that surprises everyone:** the SLIC's internal logic runs from
the **PCM clock**. With no PCM clock it can't even be initialised, so the audio
driver must be up before the control side can do anything (doc 09 §5.2–5.4).

---

## 2. Components

| # | Component | Where it lives | Responsibility | Talks to |
|---|---|---|---|---|
| 1 | **Le9641 SLIC** | on the board | line power, ringing, hook detection, analog↔digital audio, tone generator | SPI (control), PCM (audio) |
| 2 | **GPIO 32 `slic_clk_switch`** | on the board | routes the SLIC's PCM bus to the router (0) or the modem (1) | set by component 4 |
| 3 | **`slic_spi_cs1.ko`** | `fxs/kmod-slic-mt7621/src/` | registers a device on SPI chip-select 1 at runtime so the stock `spidev` driver creates `/dev/spidev0.1` | kernel SPI core |
| 4 | **`mt7621_pcm_raw.ko`** | same | claims GPIO 32 (=0); programs the audio PLL, pin mux and PCM controller as bus master; moves samples between `/dev/slicpcm` and the hardware FIFO every 250 µs | PCM hardware, userspace |
| 5 | **VP-API-II (Microsemi)** | fetched into `fxs/slicctl/vpapi/` (not in git) | the chip's "driver": init, calibration, line states, tones, events | component 6 via `VpMpiCmd()` |
| 6 | **HAL: `vp_hal.c`, `sys_service.c`** | `fxs/slicctl/arch/` | the only hardware access VP-API needs: turns its byte commands into spidev transfers; trivial locking and delays | `/dev/spidev0.1` |
| 7 | **Line profiles** | generated `fxs/slicctl/stock_profiles.c` (not in git) | Tozed's settings for this board (clock, buck-boost supply, DC feed, ringing, dial tone) | loaded by VP-API at init |
| 8 | **`slicctl`** | `fxs/slicctl/slicctl.c` | CLI test tool **and** the long-running `daemon`: owns the SLIC, runs the phone state machine, ring cadence, caller-ID timing | 5, `/dev/slicpcm`, control FIFO |
| 9 | **Caller-ID generator** | `fxs/slicctl/cid.c` | builds MDMF messages; renders Bell 202 / V.23 FSK or DTMF as audio samples | used by 8 |
| 10 | **`slic-fxs` package** | `fxs/slic-fxs/` | installs 8, the init script, uci config, `slic-play`, test clips | procd |
| 11 | **Init script** | `fxs/slic-fxs/files/slic.init` | loads 3+4 late in boot behind a crash failsafe, then starts the daemon | procd, uci |
| 12 | **Build** | `fxs/build.sh` | builds all three packages through the OpenWrt SDK, using this checkout as a feed | OpenWrt SDK |
| 13 | **Tools** | `fxs/tools/` | profile extraction, test audio, PCM analysis, caller-ID decoding, clip player, DTMF test suite, AudioSocket demo app | PC and router |
| 14 | **DTMF detector** | `fxs/slicctl/dtmf.c` | hears keypad digits in the handset audio (Goertzel, 12.75 ms blocks) | used by 8 |
| 15 | **AudioSocket server** | `fxs/slicctl/asock.c` | frames call audio between the daemon and Asterisk over TCP `127.0.0.1:4573` | used by 8; Asterisk connects |
| 16 | **Dial hook** | `fxs/slic-fxs/files/slic-dial` | turns dialled digits into an Asterisk call file | run by 8; Asterisk's `pbx_spool` |
| 17 | **PBX setup** | `fxs/asterisk/` → `/usr/sbin/slic-asterisk-setup` | installs Asterisk modules, LAN-only config, SIP passwords (doc 11) | apk, Asterisk |

---

## 3. Interfaces (the contracts)

These are the boundaries other code depends on. Change them deliberately.

### 3.1 Control FIFO `/var/run/slic.ctl` (write-only, text, one command per line)

| Command | Effect | Accepted when |
|---|---|---|
| `ring SECS [NUMBER [NAME]]` | ring for SECS (default 30) with 2 s on / 4 s off; with a number, send caller ID | on-hook |
| `talk` | line to TALK: PCM audio flows to/from the handset; tone off | any state |
| `release` | back to dial tone (if off-hook) or standby | any state |
| `stop` | same as `release` | any state |
| `cidfmt bell\|v23\|dtmf` | caller-ID format until the daemon restarts | any state |
| `cidsend NUMBER [NAME]` | caller-ID burst only, no ringing (test) | idle, on-hook |
| `call UUID SECS NUMBER\|- [NAME…]` | ring for an Asterisk call; answering bridges it over AudioSocket (doc 11 §5.2). NAME may contain spaces; `-` = no number | idle, on-hook; else writes `UUID_busy` |
| `hangup UUID` | end that call (the caller gave up): stops the bell | that call in progress |

NUMBER: digits, up to 20. NAME: up to 15 characters; for `ring`, **no spaces**
(words are split on spaces). `talk` and `release` are ignored while an
Asterisk call is in progress. Unknown commands are logged and ignored.
Commands are not acknowledged: read the state file, the call file or the log.

The FIFO is `root:asterisk` mode `0620` when an `asterisk` group exists (so
Asterisk can send `call`/`hangup`), otherwise `0600`.

```sh
echo "ring 20 0771234567 ALICE" > /var/run/slic.ctl
```

### 3.2 State file `/var/run/slic.state` (read-only, rewritten on each change)

```
state=idle|dialtone|dialing|ringing|talk|busy
hook=on|off
```

The phone LED (`/sys/class/leds/blue:phone`) mirrors `hook`.

**Call file `/var/run/slic.call`**: the current Asterisk call as
`<uuid>_<phase>`, phase = `ringing`, `answered`, `bridged`, `connecting`
(dialled, waiting for Asterisk), `ended` or `busy`. Asterisk's dialplan
waits on it (doc 11 §5.2).

### 3.3 Audio device `/dev/slicpcm`

| Property | Value |
|---|---|
| Format | signed 16-bit little-endian, mono, **8000 Hz** (`s16le`) |
| write | queues samples for the handset; blocks while the 4096-sample (0.51 s) queue is full (`O_NONBLOCK`: `EAGAIN`); the driver plays silence when the queue is empty |
| read | returns samples from the handset microphone (same format); blocks until data (`O_NONBLOCK`: `EAGAIN`) |
| poll | readable when microphone samples wait; writable when ≥ 160 samples (20 ms) fit |
| ioctl | `FIONREAD` / `TIOCOUTQ`: bytes waiting in / queued out; `TCFLSH` (`TCIFLUSH`, `TCOFLUSH`, `TCIOFLUSH`): empty a queue |
| Audible only when | the daemon has put the line in `talk` (or OHT during caller ID) |
| Multiple writers | not arbitrated: one producer at a time. The daemon keeps the device open and writes to it during calls, busy tone and caller ID; `slic-play` is safe only when no call is up |

Convert any clip: `ffmpeg -i in.mp3 -ac 1 -ar 8000 -f s16le out.s16`.

### 3.4 Configuration (`/etc/config/slic`, uci)

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `0` = don't load modules or start the daemon |
| `clk_switch` | `0` | GPIO 32: `0` router PCM (required for audio and caller ID), `1` modem PCM |
| `spi_hz` | `1000000` | SPI clock for the SLIC |
| `cid_format` | `bell` | `bell` (FSK after 1st ring), `v23` (FSK before), `dtmf` (DTMF before) |
| `audiosocket` | `127.0.0.1:4573` | where the daemon accepts Asterisk's AudioSocket connections; keep it on loopback; `off` disables calls |
| `dial_timeout` | `3` | seconds without a key that end dialling (`#` dials at once) |

### 3.5 Kernel module parameters

`mt7621_pcm_raw`: `clk_switch` (0), `setup_pll` (1), `freqin` (294000000 Hz),
`bclk_hz` (2048000), `slot_mode` (3 = 32 slots/frame), `ts_start` (1, bit offset),
`pack` (1), `pack_hi_first` (0), `tick_us` (250). Debug:
`/sys/kernel/debug/slicpcm/regs` (register dump, counters, `"<off> <val>"` to poke).

`slic_spi_cs1`: `max_hz` (1000000).

### 3.6 SPI command protocol (component 6 ↔ SLIC)

`[0x4A, EC]` (select channel; omitted when reading EC itself) → command byte
(**odd = read**) → data bytes. Chip-select held for the whole command (the
chip also accepts per-byte CS). Example: read ID = command `0x73`, 2 bytes
back: `08 49` (revision 8, product Le9641).

---

## 4. Runtime flows

### 4.1 Boot

```
procd ─► /etc/init.d/slic start (START=99, after network)
           ├─ /etc/slic.loading exists? ─yes─► log "previous load crashed", stop here
           ├─ touch /etc/slic.loading; sync
           ├─ insmod slic_spi_cs1 max_hz=…     → /dev/spidev0.1
           ├─ insmod mt7621_pcm_raw clk_switch=0
           │     GPIO 32 := 0 → PCM clock gate on → reset PCM block → audio PLL
           │     → GPIO 9-12 = PCM → 2.048 MHz / 8 kHz master → 250 µs timer
           ├─ rm /etc/slic.loading; sync
           └─ procd runs: slicctl daemon  (env SLIC_CID_FMT, respawn)
                 ├─ read ID 0x73 → expect PCN 0x49, else refuse
                 ├─ VpMakeDeviceObject(887 = tracker) + line object
                 ├─ VpInitDevice(stock profiles) … DEV_INIT_CMP data 0x0000
                 ├─ VpCalLine … CAL_CMP
                 ├─ codec = 16-bit linear, timeslot 0
                 └─ FIFO /var/run/slic.ctl, state idle, line STANDBY
```

### 4.2 Lift the handset → dial tone

```
phone off-hook → loop current → SLIC hook bit
daemon (10 ms poll loop) → VpGetEvent: HOOK_OFF
  → state idle→dialtone: line TALK + tone generator (450 Hz stock profile), LED on
    (TALK, not ACTIVE: the codec must run so the DTMF detector hears the keys)
first key → state dialing, tone off; '#' or 3 s quiet → dial (doc 11 §5.3)
phone on-hook → HOOK_ON → state idle: tone off, line STANDBY, LED off
```

### 4.3 `ring 20 0771234567 ALICE` with Bellcore caller ID

```
t=0.0  ring command   → state ringing, line RINGING
t=2.0  cadence        → line STANDBY (ring off); schedule caller ID at +0.6 s
t=2.6  caller ID      → line OHT (on-hook transmission)
                        cid_build_mdmf + cid_fsk → ~0.65 s of samples → write /dev/slicpcm
t≈3.5  burst done     → line STANDBY (end = start + burst length + 0.3 s margin)
t=6.0  cadence        → RINGING … 2 s on / 4 s off until t=20 → state idle
any time: HOOK_OFF while ringing → caller-ID timers cancelled → state talk ("answered")
```

For `v23`/`dtmf`: polarity reversal (OHT_POLREV) → 0.3 s → burst → normal
polarity → **then** the first ring.

### 4.4 Play a clip (`slic-play clip.s16`)

```
slic-play ─► "talk" → daemon: line TALK
          ─► cat clip.s16 > /dev/slicpcm  (driver queue → FIFO → PCM bus → SLIC → handset)
          ─► sleep 1 (drain the 0.5 s queue)
          ─► "release" → dial tone if still off-hook, else standby
```

---

## 5. Daemon state machine

| From | Event | To | Line state |
|---|---|---|---|
| idle | handset lifted | dialtone | TALK + tone |
| idle | `ring …` / `call …` | ringing | RINGING / STANDBY cadence, OHT for caller ID |
| idle | `talk` | talk | TALK |
| dialtone | handset down | idle | STANDBY |
| dialtone | `talk` | talk | TALK |
| dialtone | first key | dialing | TALK, tone off |
| dialtone | 30 s without a key | busy | TALK + busy tone |
| dialing | `#` or 3 s quiet | talk (dial hook runs, Asterisk connects) | TALK |
| dialing | handset down | idle | STANDBY |
| ringing | handset lifted | **talk** (answered) | TALK |
| ringing | ring time over / `hangup UUID` | idle (or dialtone if off-hook) | STANDBY |
| talk | `release` | dialtone if off-hook, else idle | |
| talk | handset down | idle (Asterisk told: AudioSocket hangup) | STANDBY |
| talk (call) | far end hangs up; hook fails; Asterisk doesn't connect (10 s dialled, 5 s answered) | busy | TALK + busy tone |
| busy | handset down | idle | STANDBY |
| any | SIGTERM (service stop) | — | STANDBY, LED off, FIFO removed |

The daemon is **single-threaded**: one `poll()` loop with a 10 ms tick waits
on the FIFO, the AudioSocket listener and call socket, and the audio device;
each pass also reads SLIC events, advances timers (cadence, caller ID, dial
timeout, busy tone) and reaps the dial hook. VP-API isn't thread-safe as
configured, so nothing else may touch `/dev/spidev0.1` while it runs.

---

## 6. Timing budget

| Quantity | Value | Why it matters |
|---|---|---|
| PCM bit clock | 2.048 MHz (= 294 MHz PLL / 2 / 71.78) | must match the SLIC's device profile, or init fails with CFAIL |
| Frame rate | 8 kHz, 32 slots of 8 bits | one 16-bit sample per direction per frame |
| Hardware FIFO | 8 words × 2 samples = 16 samples = **2 ms** | why the refill timer must be fast |
| Refill timer | 250 µs (hrtimer, PIO) | refills long before the 2 ms FIFO runs dry |
| Driver queue | 4096 samples = **0.51 s** | `write()` latency; drain time before `release` |
| SLIC poll | 10 ms loop tick | hook detection latency (well below human perception) |
| DTMF block | 102 samples = 12.75 ms; 2 agreeing blocks to report | catches the 40 ms minimum key press |
| End of dialling | 3 s after the last key, or `#` | `dial_timeout` |
| Call audio frames | 20 ms (320 bytes) each way | AudioSocket framing |
| Queue cap toward the phone | 100 ms (1600 bytes): newer frames dropped | bounds delay if Asterisk's clock runs faster than the PCM clock |
| Ring cadence | 2 s on / 4 s off | daemon timer; ring frequency and voltage come from Tozed's ring profile (not decoded) |
| Caller ID (Bellcore) | starts 0.6 s after ring 1, ~0.6–0.7 s long (0.68 s for 10 digits + 6-letter name) | GR-30 needs ≥ 0.5 s after the ring and done ≥ 0.2 s before the next |
| Init + calibration | ~1.5 s | the daemon is ready ~2 s after start |

---

## 7. Design decisions (and the alternatives we rejected)

| # | Decision | Alternatives | Why this one | Consequence / cost |
|---|---|---|---|---|
| D1 | **VP-API in userspace** over spidev | port VP-API into a kernel driver; write our own register driver | no register map is public (datasheet documents only the API); userspace bugs can't crash the router; fast iteration | a daemon owns the chip; needs Microsemi code fetched at build time |
| D2 | **Register the SPI device at runtime** (`slic_spi_cs1`) | change the device tree and rebuild/reflash firmware | no reflash; works on the stock OpenWrt image | a small, non-upstreamable module |
| D3 | **PIO + hrtimer** for PCM, raw char device | ASoC/ALSA driver with DMA (the 2020 patch) | shortest path to working audio; easy to debug; register poking via debugfs | ~4000 wakeups/s; not usable by standard ALSA tools |
| D4 | **Tozed's own line profiles**, extracted from stock firmware | Microsemi reference profiles | the board's buck-boost supply matches none of the references; the SLIC drives its own high-voltage regulator | needs a stock firmware dump to build |
| D5 | **Late module load + crash marker**, not `/etc/modules.d` | autoload at early boot | kernel has `panic_on_oops=1`, `panic=3`: an early crash would boot-loop the router and kill the internet | phone port comes up ~last at boot |
| D6 | **Text FIFO + state file** for control | ubus, unix socket, HTTP | trivial for shell scripts and any language; no dependencies | no replies; one controller at a time |
| D7 | **Caller ID generated in software** over PCM | VP-API sequencer (`vp886_seq.c`) | the sequencer isn't in the public API; the audio path was already proven | timing is ours to get right; v23/dtmf unconfirmed |
| D8 | **GPIO 32 owned by the PCM module** | `gpioset` from userspace | nobody to babysit; the pin can't drift | modem (VoLTE) audio path unavailable while loaded with `clk_switch=0` |
| D9 | **Polled events** (simple polled mode) | interrupt-driven | the SLIC's interrupt pin's GPIO (if any) is unknown | ~20 ms latency, a little CPU |
| D10 | **Three packages + an SDK feed**; vendor code and Tozed data kept out of git | ship binaries; commit everything | licensing: Microsemi code has no redistribution licence; profile bytes are firmware data | everyone builds their own |
| D11 | **AudioSocket to Asterisk**, the daemon as the TCP server | chan_dahdi, an ALSA driver, a custom channel driver, baresip on the router | ships with Asterisk; daemon stays the only owner of SLIC and audio; no Asterisk-ABI code | the handset's call leg can't carry ringing/busy signals (sounds instead); keypresses during a call reach Asterisk only as audio |
| D12 | **DTMF detected in software** on the audio | VP-API's DTMF detection | the SLIC's detector needs register sampling at 4 kHz over SPI | the line must be in TALK during dial tone |

---

## 8. Failure modes: symptom → cause → check

| Symptom | Likely cause | Check / fix |
|---|---|---|
| No `/dev/spidev0.1` | `slic_spi_cs1` not loaded; `kmod-spi-dev` missing | `lsmod`, `dmesg \| grep SLIC` |
| `slicctl id` shows PCN 0xFF/0x00 | no SPI answer: wrong CS, SLIC unclocked | is `mt7621_pcm_raw` loaded? GPIO 32? |
| Init: `DEV_INIT_CMP data=0x0003` | clock fault (CFAIL) | PCM driver, `setup_pll=1`, `clk_switch` |
| Init: `VpInitDevice failed: 21` (ERR_SPI) | SLIC has no clock at all | same as above |
| Service running, no dial tone | daemon crashed/respawning; line in wrong state | `logread -e slicctl`, `/var/run/slic.state` |
| Clip plays at double speed / wrong pitch | sample packing parameters changed | `pack=1`, `pack_hi_first=0` |
| Clip silent | line not in `talk` | `echo talk > /var/run/slic.ctl`; state file |
| Caller ID not shown | format mismatch; level | `cidfmt v23`/`dtmf`; `SLIC_CID_LEVEL`; capture + `cid_decode.py` |
| Router came up without the phone port | crash marker present | `logread \| grep slic`; `rm /etc/slic.loading` |
| After an OpenWrt upgrade, modules won't load | kernel changed; kmods are tied to the exact build | rebuild with the matching SDK |
| Lift handset: dial tone, but dialling gives busy tone | dial hook failed, Asterisk not running, or AudioSocket off | `logread -e slicctl` (`dial hook failed`, `cannot place a call`); `pidof asterisk` |
| Digits not detected | line not in TALK during dial tone; level | `logread -e slicctl \| grep digit`; record with `head -c 32000 /dev/slicpcm` and run `slicctl dtmfdet < cap.raw` on the PC |
| Calls to 100 get busy at once | phone not idle, or the FIFO isn't group-writable by asterisk | `cat /var/run/slic.state`; `ls -l /var/run/slic.ctl` (expect `prw--w---- root asterisk`) |
| `build.sh` built stale code | SDK feed linked to another checkout | fixed: `build.sh` aborts unless the feed resolves to this checkout |

Logs: `logread -e slicctl` (daemon), `dmesg | grep -E 'slicpcm|SLIC'`
(modules), `cat /sys/kernel/debug/slicpcm/regs` (PCM counters: `tx_unrun`
must stay 0).

---

## 9. Hands-on labs (for a new developer)

Each lab is safe on a working router. The line carries up to ~100 V when
ringing: don't touch the RJ11 contacts.

1. **See the state**: `cat /var/run/slic.state`; lift the handset; `cat` again;
   `logread -e slicctl | tail`.
2. **Ring it**: `echo "ring 8" > /var/run/slic.ctl`. Watch the state change to
   `ringing` and back.
3. **Caller ID offline**: on the PC, `slicctl cidgen bell 0123456789 TEST > c.s16`
   (host build: `make -C fxs/slicctl`), then `python3 fxs/tools/cid_decode.py c.s16 bell`.
   Change the number; break a byte in `c.s16`; watch the checksum fail.
4. **Caller ID for real**: `echo "ring 12 0123456789 TEST" > /var/run/slic.ctl`.
5. **Audio**: lift the handset, `slic-play /usr/share/slic/tone_1khz_5s.s16`.
6. **Measure what you hear**: in `talk`, record the line echo with
   `head -c 32000 /dev/slicpcm > cap.raw`, copy to the PC, run
   `fxs/tools/pcm_analyze.py cap.raw` (dominant frequency, purity).
7. **Read the code in this order**: `slicctl.c` `cmd_daemon()` → `cid.c` →
   `vp_hal.c` → `mt7621_pcm_raw.c` `pcm_tick()` → `slic.init`.
8. **Rebuild and redeploy**: doc 09 §12 / `fxs/README.md`.

---

## 10. Extension points (building applications)

The next stage is applications on top of this platform. What they can rely
on:

| Need | Use | Notes |
|---|---|---|
| Ring the phone, optionally with a number | `ring SECS NUMBER NAME` on the FIFO | the daemon handles cadence and caller-ID timing |
| Know when the phone is picked up / hung up | poll `/var/run/slic.state` (or watch `logread -f -e slicctl`) | event push (ubus/socket) is a sensible next addition |
| Play audio to the handset | `talk`, write s16le 8 kHz to `/dev/slicpcm`, `release` | one writer at a time |
| Hear the handset microphone | read `/dev/slicpcm` while in `talk` | echo of what you play is cancelled in the driver (OSLEC, `ec=Y`; doc 11 §7.1) |
| Dial digits from the phone | Asterisk: dialled numbers run in `[from-phone]` (doc 11) | without Asterisk, digits only appear in the log |
| Full calls (SIP, apps, AI agents) | Asterisk extension 100 / 800 (doc 11) | AudioSocket for apps: 8 kHz s16le, 20 ms frames |

Candidate applications and what each still needs:

| Application | Missing pieces |
|---|---|
| **Notifier** (ring with caller ID when something happens, e.g. a server alert or doorbell) | nothing: works today |
| **Announcement player / intercom** (lift the handset, hear TTS or a message) | a TTS → s16le step; hook event triggering |
| **SIP phone** | done with Asterisk on the LAN (doc 11); an ISP SIP trunk is the remaining step (doc 11 §10); line echo cancelled in the driver since kmod r3 |
| **AI agent / voice app** | works through extension 800 (doc 11 §8); line echo cancelled in the driver (doc 11 §7.1), off-hook result still to measure |
| **Modem calls** (GSM voice through the LTE modem) | `clk_switch=1` path and the modem's audio routing (doc 07); VoLTE allow-list limits (doc 09 §3.4) |

Architectural gaps worth closing before bigger applications:

1. **Event push**: a ubus object (`slic`) with methods (`ring`, `talk`, …) and
   events (`hook`, `state`), replacing FIFO polling for richer clients.
2. **Audio arbitration**: a single audio owner, or an ALSA PCM device, so two
   apps can't write `/dev/slicpcm` at once.
3. ~~**DTMF input**~~: done, software detector (doc 11 §6).
4. ~~**Echo cancellation**~~: done with the kernel's OSLEC in the PCM driver (doc 11 §7.1), where Tozed used WebRTC AECM in userspace.

---

## 11. Glossary

| Term | Meaning |
|---|---|
| **FXS** | Foreign eXchange Station: the side of a phone line that powers and rings a phone (what this jack is) |
| **SLIC** | Subscriber Line Interface Circuit: the chip implementing an FXS line (here: Le9641) |
| **BORSCHT** | the SLIC's jobs: Battery, Overvoltage, Ringing, Supervision, Codec, Hybrid, Test |
| **Tracker** | a SLIC supply design whose battery voltage tracks what the line needs (saves power); the Le9641 is one ("887 series" in the API) |
| **Hook / off-hook** | handset lifted (loop current flows) / put down |
| **Hook flash** | a short on-hook pulse used as a "feature" key (call waiting, transfer); the test phone sends 600 ms by default |
| **REN** | Ringer Equivalence Number: how many bells a line can ring (Le9641: up to 5) |
| **PCM bus** | serial audio bus: bit clock + frame sync + data, here 2.048 MHz / 8 kHz |
| **Timeslot** | an 8-bit slot inside a PCM frame; we use slot 0 (16-bit linear takes two) |
| **MPI** | Microsemi's name for the SLIC's SPI command interface |
| **EC register** | "enable channel": selects which line a command applies to |
| **VP-API-II** | Microsemi's C library for its SLICs ("VoicePath API") |
| **Profile** | a byte array configuring the SLIC (device, DC feed, ringing, tone, …) |
| **CFAIL** | clock-fail flag: the SLIC isn't getting a usable PCM clock |
| **OHT** | On-Hook Transmission: line state that passes audio to an idle phone (needed for caller ID) |
| **Polarity reversal** | swapping the line's two wires' voltage; ETSI/DTMF caller ID uses it as an alert |
| **FSK** | Frequency-Shift Keying: data as two tones (mark = 1, space = 0) |
| **Bell 202 / V.23** | the two FSK tone pairs used for caller ID (1200/2200 Hz vs 1300/2100 Hz) |
| **MDMF** | Multiple Data Message Format: caller-ID message with tagged fields (date/time, number, name) |
| **GR-30** | Telcordia (Bellcore) caller-ID standard (signal after the first ring) |
| **kfifo** | Linux kernel ring buffer (our 0.51 s sample queue) |
| **PIO** | Programmed I/O: the CPU copies data itself (vs DMA) |
| **procd** | OpenWrt's init/service supervisor |
| **apk** | OpenWrt 25.x package manager |
| **DTMF** | Dual-Tone Multi-Frequency: the keypad's tone pairs |
| **Goertzel filter** | a cheap way to measure one frequency's strength in a block of samples |
| **AudioSocket** | Asterisk's simple TCP audio protocol (doc 11 §8.1) |
| **PBX** | Private Branch eXchange: a phone exchange (here Asterisk) |

## How obtained

Assembled from the code in `fxs/` (2026-10-11) and the verified results in
docs 09 and 11. Every timing and interface value above was read from the source or
measured on the router in doc 09's experiments.
