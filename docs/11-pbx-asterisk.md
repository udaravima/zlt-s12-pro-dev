# 11 — Home PBX: Asterisk on the router, the RJ11 phone as extension 100

Written 2026-10-11. Follows [09-phone-port-fxs.md](09-phone-port-fxs.md) (how
the phone port was revived) and [10-phone-port-architecture.md](10-phone-port-architecture.md)
(how its parts fit). Code: [`fxs/asterisk/`](../fxs/asterisk), the daemon in
[`fxs/slicctl/`](../fxs/slicctl), the dial hook in `fxs/slic-fxs/files/slic-dial`.
Hands-on setup, configuration and connecting devices:
[12-pbx-setup-guide.md](12-pbx-setup-guide.md).

## TL;DR

The router now runs **Asterisk 23**, a full phone exchange (PBX), reachable
only from the home LAN. The analog phone on the RJ11 jack is a normal
extension on it, **100**: it can dial out with its keypad and it rings, with
caller ID, when called. Anything on the LAN can join: a SIP softphone or
desk phone logs in as **201–203**, and any program (an app, an AI agent) can
take over a call through **extension 800** using AudioSocket, a protocol
simple enough that the demo app is ~110 lines of standard-library Python.

The surprising part is how the phone gets into Asterisk. Asterisk has no
driver for this phone chip, and writing one would mean a C module tied to
Asterisk's internals. Instead, the daemon that already owns the phone
(`slicctl daemon`) became an **AudioSocket server**: Asterisk connects to it
over a local TCP socket and exchanges raw audio frames. That means the phone
side never had to learn SIP or Asterisk's APIs, and Asterisk never touches
the phone hardware.

**What you can do today (verified on the real phone, 2026-10-11):**

| From | Action | Result |
|---|---|---|
| RJ11 handset | lift | dial tone |
| RJ11 handset | dial **601** | "hello world … goodbye", then busy tone when it hangs up |
| RJ11 handset | dial **600 #** | echo test: hear yourself (both audio directions), 39 s call, 0 frames dropped |
| RJ11 handset | dial **800 #** | call handed to a program on a LAN PC; the demo app beeps and repeats your voice 1 s later |
| Asterisk | call **100** | phone rings, caller ID shown (number + name), answer → two-way audio |
| Asterisk | caller gives up while it rings | bell stops at once |
| SIP test tool (sipp) on the LAN PC | call 601 as account 202 | password challenge passed, announcement played, call completes |
| softphone on a mobile (Wi-Fi), account 201 | call **100** | handset rings, caller ID "201", answered, 24 s two-way call |
| RJ11 handset | dial **201 #** | softphone rings, handset hears ringback, 17 s two-way call |

**Not yet verified:** registering more than one device per account (the
config allows 2), and the Sri Lanka ringback/busy tones (§2) on a live call.

---

## 1. The shape of it

```
 LAN (192.168.x.0/24)                    Router
 ┌──────────────────────┐   SIP 5060    ┌───────────────────────────── Asterisk 23 ──────┐
 │ softphone / desk     ├──────────────►│ PJSIP (bound to the LAN IP only)               │
 │ phone: 201..203      │◄──RTP────────►│   endpoints 201-203, context from-lan          │
 └──────────────────────┘ 10000-10199   │                                                │
 ┌──────────────────────┐               │ dialplan [house]                               │
 │ app / AI agent       │◄──AudioSocket─┤   100 → [slic-in] → ring, answer, AudioSocket ─┼──┐
 │ (ext 800, TCP)       │   TCP         │   201-203 → PJSIP   600 echo  601 hello  800 ──┘  │
 └──────────────────────┘               │                                                │  │
                                        │ pbx_spool ◄── call file ◄── /usr/libexec/slic-dial │
                                        └────────────────────────────────────────────────┘  │
                                              AudioSocket TCP 127.0.0.1:4573 (loopback) ◄────┘
                                        ┌──────────────── slicctl daemon (root) ─────────────┐
                                        │ hook events, ringing, caller ID, DTMF detector,    │
                                        │ dial tone, busy tone; FIFO /var/run/slic.ctl       │
                                        └──── /dev/spidev0.1 (control) ── /dev/slicpcm (audio)
                                                               Le9641 SLIC ── RJ11 ── phone
```

Three pieces of glue connect the daemon and Asterisk, and each carries one
kind of thing:

| Glue | Direction | Carries |
|---|---|---|
| **AudioSocket TCP** `127.0.0.1:4573` | Asterisk connects to the daemon | the call's audio, both ways, plus "hang up" |
| **Control FIFO** `/var/run/slic.ctl` | Asterisk → daemon | `call` (ring the phone for this call) and `hangup` (the caller gave up) |
| **Call file** `/var/run/slic.call` | daemon → Asterisk | where the current call stands: `<uuid>_ringing`, `_answered`, `_bridged`, `_ended`, `_busy`, `_connecting` |

Every call carries a **UUID**, a random 36-character id that Asterisk creates
for incoming calls and the daemon creates for outgoing ones. When Asterisk
connects to port 4573, the first thing it sends is that UUID. The daemon
accepts the connection only if it is waiting for exactly that call, so
a stray or replayed connection can't hijack the phone (verified: a random
UUID is refused and the line stays idle).

---

## 2. Installing (OpenWrt 25 uses apk)

OpenWrt 25 replaced `opkg` with **apk** (Alpine's package manager, v3). The
Asterisk packages come from the official `telephony` feed and install with
plain `apk add`. Everything is wrapped in one script that ships with
`slic-fxs`:

```sh
apk update
APP=192.168.1.20:9092 slic-asterisk-setup     # APP = where ext 800 sends calls
```

It does five things, in order, and is safe to re-run:

1. `apk add` the 32 Asterisk packages the dialplan needs. Each one is in the
   list because something uses it: `pjsip` for SIP, the three `audiosocket`
   modules, `pbx-spool` (call files), `app-system` (talking to the FIFO),
   `app-waitforcond`, `func-shell`, `codec-gsm` (the bundled sounds are GSM
   files: without it `Playback()` fails with "no translation path gsm → ulaw").
2. Find the LAN address from the firewall zone named `lan`, so nothing is
   hard-coded.
3. Fill the templates in `/usr/share/slic/asterisk/` into `/etc/asterisk/`
   (`pjsip.conf`, `extensions.conf`, `rtp.conf`). A changed existing file is
   kept as `*.bak-DATE`.
4. Accounts: keep every login already in `/etc/asterisk/pjsip_secrets.conf`,
   add any in `ACCOUNTS` (201–203 on a first run) with a random 20-character
   password, write the file (owner `asterisk`, mode 0600) and generate
   `pjsip_accounts.conf` from it. `--new-passwords` replaces them all.
   **The secrets file is never in git.**
5. Enable and restart Asterisk, restart the phone daemon (so its FIFO gets
   the `asterisk` group), print the accounts.

Options: `--no-apk` (modules already installed), `--new-passwords`,
`ACCOUNTS="204 …"` (add SIP logins), `TONES=xx` (another country's tones,
below). Re-runs keep passwords and the app address. Every knob is in doc 12.

**Call-progress tones.** Ringback ("it's ringing"), busy and congestion are
sounds Asterisk generates from `indications.conf`, and its default is the
US set: a single long 440+480 Hz burst every 6 s. Asterisk ships no Sri
Lanka entry, so the setup appends one (`fxs/asterisk/indications-lk.conf`)
and sets `country=lk`. The values come from ITU's national tones list
(Operational Bulletin 781 annex, E.180, position 1 Feb 2003):

| Tone | Sri Lanka (ITU) | Asterisk default (us) |
|---|---|---|
| ringback | 425 Hz, 0.4 on 0.2 off 0.4 on 2.0 off (double ring) | 440+480 Hz, 2 on 4 off |
| busy | 425 Hz, 0.75 on 0.75 off | 480+620 Hz, 0.5 on 0.5 off |
| congestion | 425 Hz, 0.25 on 0.25 off | 480+620 Hz, 0.25 on 0.25 off |
| dial | 425 Hz continuous | 350+440 Hz |

Two tones don't come from Asterisk. The handset's **dial tone** is the SLIC's
own generator, set by Tozed's stock tone profile (450 Hz, doc 09), 25 Hz
above the ITU value; changing it means editing profile bytes, so it's left
as Tozed shipped it. The **busy tone** the handset hears after a call ends
comes from the daemon, which uses the same Sri Lanka values. Not yet
matched: the **bell cadence** (the daemon rings 2 s on / 4 s off; Sri
Lanka's is the double ring). Changing that moves the caller-ID slot, so it
needs its own test on the phone.

**Cost on this router** (measured with nothing in progress): Asterisk uses
**16 MB resident** (40 MB virtual) of the 248 MB RAM; 132 MB stays
available. On disk: 2.4 MB of modules plus 3.0 MB of sounds, on the 7 GB USB
extroot. CPU during a call was not measured.

---

## 3. Numbering plan

| Number | What | Defined in |
|---|---|---|
| **100** | the RJ11 phone | `[house]` → `[slic-in]` |
| **201–203** | SIP accounts for LAN devices: softphones, desk phones, apps that speak SIP | `pjsip.conf` |
| **600** | echo test: you hear yourself, which proves both audio directions | `[house]` |
| **601** | "hello world … goodbye": proves the far-to-phone direction | `[house]` |
| **800** | hand the call to the AudioSocket app at `AUDIOSOCKET_APP` | `[house]` |

Both entry points (SIP accounts land in `[from-lan]`, the handset in
`[from-phone]`) include `[house]`, so everyone can dial everyone. A number
that doesn't exist should play "that is not a valid extension" to the
handset (the `i` extension; not tried yet).

---

## 4. Keeping it LAN-only

A SIP server reachable from the internet gets found by scanners within
hours, and a compromised one can run up a phone bill once a trunk exists.
Four independent layers stop that:

| Layer | Setting | Effect |
|---|---|---|
| Bind address | `transport-lan-udp/tcp: bind=<LAN IP>:5060` | Asterisk doesn't even listen on the WAN side (verified with `netstat`: only `192.168.x.254:5060`) |
| Asterisk ACL | `[lan-only] type=acl`: deny all, permit the LAN subnet and 127.0.0.1 | requests from elsewhere are rejected before authentication |
| Firewall | zone `wan` input `REJECT`, no port forwards | the router drops it anyway |
| Passwords | random 20-character, per account, file mode 0600 | a LAN device still has to log in |

RTP (the audio itself, UDP 10000–10199) binds to all addresses, as Asterisk
does by default. Only the firewall keeps those ports off the WAN, so
**never add a port forward for 10000–10199 or 5060.**

**The control FIFO** is `root:asterisk`, mode `0620`: Asterisk can write
commands, nothing else unprivileged can, and nobody can read it.

**Caller ID is untrusted input.** It comes from whoever is calling, and the
dialplan passes it to a shell command (`System(echo '…' > FIFO)`). Before
that, `FILTER()` whitelists it: digits, `*#+` for the number; letters,
digits, space, `._-` for the name (15 chars max). No quote, `$`, backtick,
`;` or `|` can survive, so the single-quoted argument can't be broken out
of. Tested with hostile names.

---

## 5. How the phone joins a call

### 5.1 Why AudioSocket (and not the obvious options)

Asterisk talks to analog phones through *channel drivers*. None of the
standard ones fits this hardware:

| Option | Why not |
|---|---|
| `chan_dahdi` (the classic analog driver) | needs a DAHDI kernel driver for the card; there is none for this SLIC |
| `chan_alsa` / `chan_console` | needs an ALSA sound device; our PCM driver is a raw char device (doc 10 D3) |
| a custom Asterisk channel driver | C code compiled against Asterisk's internal ABI; breaks on Asterisk upgrades |
| a SIP client (baresip) on the router driving `/dev/slicpcm` | a second program owning the audio device, plus SIP on loopback; more moving parts, and the hook/ring logic would be split between two daemons |
| **AudioSocket** (chosen) | ships with Asterisk; plain TCP with 3-byte frame headers; the daemon stays the **only** owner of the SLIC and the audio device |

### 5.2 A call to the phone (inbound)

The catch: `chan_audiosocket` *answers* as soon as its TCP connection opens.
If Asterisk dialled the daemon directly, every call would count as answered
before the phone even rang, and the caller would never hear ringing. So the
ringing is done by the dialplan, and AudioSocket is opened only once the
handset is lifted:

```
caller ──Dial(Local/ring@slic-in)──► [slic-in] ring:
  1. phone not idle?  (FILE(/var/run/slic.state) != "state=idle")  → Busy
  2. UUID = new random id; NUM, NAME = FILTER(caller ID)
  3. System(echo 'call UUID 45 NUM NAME' > /var/run/slic.ctl)
                                   daemon: state ringing, 2 s on / 4 s off,
                                   Bellcore caller ID between rings 1 and 2,
                                   writes /var/run/slic.call = UUID_ringing
  4. Ringing()   → the caller hears ringback
  5. WaitForCondition: slic.call becomes UUID_answered | _ended | _busy
     (checked every 0.1 s, up to 45 s)
                                   handset lifted → daemon: state talk,
                                   slic.call = UUID_answered, waits ≤ 5 s
  6. Answer(); AudioSocket(UUID, 127.0.0.1:4573)
                                   daemon: UUID matches → slic.call = UUID_bridged;
                                   audio flows both ways
  … either side hangs up → the socket closes → the other side ends
  h extension: the caller left early → echo 'hangup UUID' → bell stops
```

Measured on the real phone (incoming test call, 2026-10-11):

| Time | Event |
|---|---|
| 18:31:45 | `call` command received, ringing starts |
| 18:31:48 | caller ID sent between rings 1 and 2 (0.71 s burst) |
| 18:31:57 | handset lifted → answered and bridged within the same second |
| 18:32:25 | hung up after 28 s, 0 frames dropped |

A detail that matters: step 5 waits for *this* call's UUID with one of three
outcomes, not for "not ringing". Until the daemon reads the command, the
file still holds the previous call's outcome, and a looser test would
misread it.

### 5.3 A call from the phone (outbound)

```
handset lifted → daemon: state dialtone
    line in TALK (not ACTIVE: the codec must run so the keypad tones reach us)
    + the SLIC's own 450 Hz dial-tone generator
first key → state dialing, dial tone off
    DTMF detector on the incoming audio (§6) collects digits
'#' or 3 s without a key → end of number
    UUID = new id; slic.call = UUID_connecting; state talk
    run /usr/libexec/slic-dial UUID DIGITS     (no shell: execl, digits 0-9*#A-D)
        writes a call file and mv's it into /var/spool/asterisk/outgoing/:
            Channel: AudioSocket/127.0.0.1:4573/UUID/c(slin)
            Context: from-phone   Extension: DIGITS
Asterisk (pbx_spool) opens the AudioSocket channel → connects to the daemon
    daemon: UUID matches → bridged
    the channel is answered; Asterisk runs [from-phone] DIGITS
```

Because the handset's leg is already "answered", it can't receive call
progress *signals* (ringing, busy) the way a SIP phone does. So the dialplan
gives it *sounds*: `Dial(..., r)` plays ringback audio into the call while
a SIP extension rings, and when the far end hangs up or is busy, the daemon
plays its own **busy tone** (Sri Lanka's: 425 Hz, 0.75 s on / 0.75 s off,
for 30 s).

The busy tone also covers every way an outgoing call can fail: the dial hook
fails, Asterisk doesn't connect within 10 s, or the handset sits off-hook
for 30 s without dialling.

### 5.4 The control commands Asterisk uses

Added to the FIFO (full table in doc 10 §3.1):

| Command | Meaning |
|---|---|
| `call UUID SECS NUMBER\|- [NAME words…]` | ring for this Asterisk call with caller ID; `-` = no number. Refused (writes `UUID_busy`) if the phone isn't idle. NAME may contain spaces |
| `hangup UUID` | the caller gave up: stop ringing (or end the call). Ignored for any other UUID |

---

## 6. Hearing the keypad: the DTMF detector

Each keypad button sends two tones at once, one from a "row" group
(697/770/852/941 Hz) and one from a "column" group (1209/1336/1477/1633 Hz).
The SLIC can detect these itself, but only by sampling its registers 4000
times a second over SPI, which is far too heavy here. So the daemon listens
to the audio instead, in software (`fxs/slicctl/dtmf.c`).

It runs one **Goertzel filter** per frequency (a cheap way to measure the
strength of a single frequency) over blocks of **102 samples = 12.75 ms**.
That's the block size Asterisk's own detector uses, and the reason is a
trade-off:

- **Shorter blocks catch short presses.** The standard minimum key press is
  40 ms. Requiring 2 agreeing blocks (25.5 ms) catches it however it falls
  across block edges. An earlier 205-sample version (25.6 ms blocks) missed
  40 ms tones.
- **Longer blocks are more frequency-selective.** 102 samples gives 78 Hz-wide
  filters: wide enough to accept a phone that's 1.5 % off pitch, and narrow
  enough that the neighbouring row (697 vs 770 Hz) leaks in only at −22 dB.

A block counts as a key only if all four tests pass:

| Test | Threshold | Rejects |
|---|---|---|
| each tone loud enough | amplitude ≥ 150 of 32767 (about −47 dBFS) | line noise |
| strongest beats runner-up in its group | by 6 dB | chords, two keys at once |
| row vs column level ("twist") | within 8 dB | voice that happens to hit one frequency |
| the two tones carry most of the energy | ≥ 55 % | speech, music |

A key is reported once when 2 blocks agree, and re-armed after 2 blocks
without it, so holding a key gives one digit and pressing it twice gives two.
`fxs/tools/dtmf_test.py` runs 17 offline cases against a host build, all
passing: all 16 keys at normal and quiet (−35 dBFS) levels, too-quiet keys
(−55 dBFS) ignored, dial tone echoing back as loud as the keys, ±6 dB twist,
±1.5 % frequency error, white noise, a held key (one digit), a key pressed
three times (three digits), 60 ms and 40 ms presses, and two signals that
must produce nothing. Rejecting *speech* is what the 55 % test is for, but
no speech recording is in the suite yet. On the real phone every digit
of `601`, `600#` and `800#` was detected.

**Limitation:** after the call connects, keys pressed on the handset travel
only as *sound* in the audio. The AudioSocket protocol carries key events
from Asterisk to the client, never back. So an app on 800 receives the
handset's keypresses as tones it would have to detect itself. A SIP phone
using RFC 4733 *does* deliver them to the app as events (frame type `0x03`).

---

## 7. Audio path details

The daemon needed a non-blocking audio device to run everything (FIFO,
socket, SLIC events, audio) in one `poll()` loop with a 10 ms tick, so the
PCM driver gained (`mt7621_pcm_raw.c`, kmod release 2):

| Addition | Use |
|---|---|
| `poll()` | readable when handset audio is waiting; writable when ≥ 160 samples (20 ms) fit |
| `O_NONBLOCK` | `read`/`write` return `EAGAIN` instead of blocking |
| `FIONREAD` / `TIOCOUTQ` ioctls | bytes waiting in each direction |
| `TCFLSH` ioctl | empty the queues, e.g. drop stale audio when a call is bridged |

**Two clocks, one drift problem.** Asterisk sends a 20 ms frame whenever its
own timer says 20 ms have passed. The phone consumes samples at the PCM
clock, made by the audio PLL. If the two clocks differ, the queue toward the
phone either grows (Asterisk faster) or runs dry (PCM faster). The daemon
caps growth at **100 ms** (1600 bytes) and counts the frames it drops; every
call so far logged **0 drops**, which looked like "no drift" but wasn't:
**the PCM clock is 0.41 % fast** (measured 2026-10-12: 80,331 FIFO words in
20.000 s by the router's NTP clock = **8,033 samples/s**, not 8,000). So the
queue never grows, it runs dry about 33 times a second, and each time the
driver plays a few samples of silence (the drop counter can't see that).
Toward the phone that's a tiny gap every few frames; from the phone, apps
receive 0.4 % more audio than real time.

The divider is computed for a 294 MHz PLL (`freqin`): 147 MHz / 2.048 MHz =
71.777 (`DIVINT 71`, `DIVCOMP 199/256`). Setting **72 + 19/256 = 72.074**
through debugfs gave **8,000.0 samples/s** over 20 s with no FIFO errors
(tested live, then restored). Why it's off is not settled: either the PLL
really runs at about 295.2 MHz, or the frame is 255 bit clocks long instead
of 256 (2,048,000 / 255 = 8,031 Hz, closer to the nominal PLL). **Fixed in
`kmod-mt7621-pcm-raw` r3:** the default `freqin` is now the calibrated
295,216,000, which yields that divider; measured 8,000.6 samples/s after
loading it (the 0.6 is within this method's timing noise).

**Echo.** A two-wire phone line carries both directions on the same pair, so
some of what the router sends leaks back into what it receives. Measured with
noise (2026-10-12, §7.1): the echo comes back **about 20 dB below** what was
sent (an earlier single-beep estimate said 30 dB). Whoever is on the *other*
end of the call hears it: their own voice, 20 dB quieter, delayed by the
trip through the router. For an app it's a real problem: an AI agent hears a
faint copy of its own speech, which can look like the caller interrupting.
Since `kmod-mt7621-pcm-raw` r3 the driver cancels it (§7.1).

### 7.1 Echo cancellation: what it is and how we'd add it

There are two different echoes in a call, and they need cancellers in
different places:

| Echo | Where it starts | Who hears it | Who cancels it |
|---|---|---|---|
| **Acoustic** | a phone's loudspeaker is picked up by its own microphone (worst on speakerphone) | the person at the *other* end | that phone, e.g. Linphone's "echo cancellation" option |
| **Line (hybrid)** | the analog line carries both directions on one pair of wires; the SLIC separates them imperfectly, so part of what it plays leaks into what it hears | the person at the *other* end: here, the softphone user or the app | the equipment at the analog line, i.e. **this router** |

So Linphone's option is worth keeping on (it protects whoever talks to the
mobile), but it can't remove this router's line echo: that echo arrives
inside the audio Linphone receives, and Linphone's canceller only cleans
what its own microphone picks up.

An echo canceller is an adaptive filter. It keeps the last few
milliseconds of what was sent to the line, learns how the line turns that
into echo (a short impulse response), predicts the echo in what comes
back, and subtracts it. It needs two things: the sent and received samples
**aligned in time**, and an echo delay **shorter than its filter**. A
non-linear processor (NLP) then suppresses the small residue when only the
far end is talking.

**Findings (2026-10-11):**

- **No hardware fix is configured.** The SLIC can shape its own echo with
  an *AC profile* (line impedance and the trans-hybrid balance filter). We
  load none (`VpInitDevice(…, VP_PTABLE_NULL, …)`), and the stock firmware
  holds none either: every Profile Wizard header in Tozed's `mislic` is a
  device, DC, ring or tone profile. Tozed relied on a software canceller:
  its call program `tz_phone` contains WebRTC's mobile echo canceller
  (AECM), seen in the binary's strings during the 2026-10-08 firmware
  analysis. Microsemi's reference package
  has 600 Ω AC profiles for related ZL880 boards and Profile Wizard project
  files for two miSLIC boards; their coefficients depend on the board's
  protection components, so using one here would be an experiment to
  measure, not a fix.
- **The Linux kernel's line echo canceller, OSLEC, is ready to use.** It
  lives in `drivers/misc/echo`, OpenWrt builds it for this exact kernel as
  `kmod-echo` (official package), and it exports `oslec_create`,
  `oslec_update`, `oslec_flush`, `oslec_free`, `oslec_adaption_mode`,
  `oslec_hpf_tx`. It was written for this job (analog phone ports via
  DAHDI), runs in fixed-point (this CPU has no FPU), per sample.
- **The right place is the PCM driver's 250 µs tick** (`pcm_tick()`), where
  each sample goes to the SLIC and each sample comes back with a *fixed*
  delay of a few milliseconds. In the daemon, by contrast, the outgoing
  queue holds 0–100 ms that varies with clock drift, and a canceller can't
  track a moving delay.
- **CPU:** a 128-tap filter (16 ms of echo) costs on the order of a few
  million multiply-adds per second, under 1 % of one 880 MHz core
  (estimate, not measured).

**Measurement (2026-10-12).** `fxs/tools/echo_measure.py` plays a fixed
signal (1 s silence, 4 s white noise at −20 dBFS, 1 s silence, 4 s at
−10 dBFS) while the handset is lifted and silent, and fits a least-squares
filter to the echo: the best a *linear* canceller could do.

| Where measured | Echo below sent (ERL) | Echo delay | Best linear cancellation (ERLE) |
|---|---|---|---|
| App side, through ext 800 | 20 dB | 50 → 76 ms **during 9 s** | **0 dB**: the delay moves (PCM clock 0.41 % fast) and jitters by about 4 samples |
| Router side, `/dev/slicpcm` played and recorded together | 20.3 dB at both levels (linear) | **constant** to the sample | **19–22 dB** (residual −59 / −52 dBFS) |

What that means for a canceller:

- **It must sit at the driver, not in an app.** On the same PCM clock the
  echo path is fixed; across the daemon's queue and the network it moves,
  and no adaptive filter can follow it.
- **The echo is short: 99 % of its energy lies within 2.1 ms** (17 taps).
  The delay in the router-side recording includes the driver's queues, so
  the delay `pcm_tick()` will see is not measured, but the FIFOs are only 8
  words deep, so it is a few ms: a 128-tap (16 ms) OSLEC filter covers it.
- **Expect about 20 dB from the linear filter**, so about 40 dB in total
  with the line's own 20 dB. The residual grows with the signal level
  rather than sitting at the −70 dBFS noise floor, so something non-linear
  or not quite stationary remains (not identified). OSLEC's non-linear
  processor suppresses such residue while only the far end talks.

**What's built (kmod-mt7621-pcm-raw r3, 2026-10-12).** `pcm_tick()` keeps
a 1,024-sample history of everything written to the TX FIFO and passes every
sample read from the RX FIFO through `oslec_update()` with its reference:
the sample written in the same tick (`ec_pre = 0`). An echo can only come
back *after* its sample was written, so the echo lands at a positive tap: the
FIFO latency plus the SLIC's own delay, well inside 128 taps. The canceller
restarts whenever the receive queue is flushed (`TCIFLUSH`, which the daemon
does as each call connects) or when it is switched back on.

| Module parameter | Default | Meaning |
|---|---|---|
| `ec` | `Y` | cancel echo; writable at runtime in `/sys/module/mt7621_pcm_raw/parameters/` |
| `ec_mode` | `43` (0x2b) | OSLEC flags: 1 adapt, 2 NLP, 8 clip residue to the background level, 0x20 RX high-pass (cuts DC and below ~160 Hz) |
| `ec_pre` | `0` | pair each received sample with the sample sent this many samples earlier; raise only if the echo delay ever exceeds `ec_taps` |
| `ec_taps` | `128` | filter length in samples (16 ms); load time only |

`debugfs slicpcm/regs` prints a line `ec on taps 128 mode 0x2b pre 0 ...`.

**A trap in the first version:** `ec_pre` started at 64, on a sign error.
Pairing each received sample with the sample sent 64 samples *earlier* than
the same tick put the echo before its own cause, outside the filter, so
OSLEC learned nothing. And the NLP only engages once the total echo loss
(line plus filter) reaches 24 dB; the line alone gives 20, so with the filter
idle the NLP never woke either. The test said "on" and "off" were identical
(20.4 vs 19.8 dB), which is what exposed it.

**Results** (the same noise at −20 dBFS, canceller off → on in one run;
played and recorded at `/dev/slicpcm`, as in the measurement above):

| Setup | Canceller off | On, filter only (no NLP) | On, full (NLP) |
|---|---|---|---|
| Handset **on the cradle**, line in TALK (strong echo) | 6.7 dB down | **33 dB** down (−53 dBFS); learned within 0.25 s | about **44 dB** down (−64 dBFS) |
| Handset **lifted** (a real call) | 20.4 dB down | not yet measured with `ec_pre = 0` | not yet measured |

The on-cradle case verifies the canceller itself and was repeated after a
reload with the new defaults (same numbers). The off-hook run with the
correct alignment is still to do, as is a listening check on a softphone
call. The cradle case is also the harder one: its echo is 14 dB stronger.

**Levels** (same 800 test): speech arrived at −28 to −36 dBFS RMS, with
−57 to −60 dBFS between words. That is normal telephone level with a quiet
line, and fine for speech-to-text without extra gain.

---

## 8. Building an app on extension 800

Extension 800 answers and hands the call to `AUDIOSOCKET_APP`, set by
`APP=host:port slic-asterisk-setup`. **Asterisk is always the TCP client**:
your program listens, and Asterisk connects once per call.

### 8.1 The protocol

Every message is a frame: `kind` (1 byte) · `length` (2 bytes, big-endian) ·
`payload`.

| Kind | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | UUID | Asterisk → app, always first | 16 raw bytes |
| `0x10` | audio | both | signed 16-bit little-endian mono, **8 kHz**; 20 ms = 320 bytes |
| `0x03` | DTMF | **Asterisk → app only** | 1 ASCII byte (`0`–`9`, `*`, `#`, `A`–`D`) |
| `0x00` | hangup | both | none |
| `0xFF` | error | Asterisk → app | error code |

Three traps, all read from Asterisk's source; the first was also hit live:

1. **2-second silence rule.** If Asterisk receives nothing from the app for
   2 s, it gives up on the call (`Reached timeout after 2000 ms of no
   activity`). The app must keep sending audio, silence included, for as
   long as the call should last. An app that only "speaks when spoken to"
   dies the moment the caller goes quiet. That happened in testing with a
   caller that sent no audio.
2. **Never send `0x03`.** Asterisk treats a DTMF frame from the app as an
   error and drops the call.
3. **The audio is 8 kHz.** Speech-to-text models trained on 16 kHz need
   resampling, and text-to-speech output must be converted down to 8 kHz.

### 8.2 The demo: `fxs/tools/audiosocket_demo.py`

A complete app in standard-library Python. It beeps, then plays everything
the caller says back 1 s later, and saves each call's incoming audio as
`audiosocket-<uuid>.wav`. Its structure is the template for a real agent:

```
receive loop:  0x01 → start the call, start the sender
               0x10 → append the caller's audio to the delay line (and the .wav)
               0x03 → a key was pressed (SIP callers only, §6)
               0x00 / 0xFF / EOF → end
sender:        every 20 ms, send 320 bytes from the output buffer, or silence
```

The sender runs on its **own 20 ms clock**, separate from receiving. That
is what keeps it within the 2-second rule, and it's where an agent plugs in:
replace the delay line with *speech-to-text → model → text-to-speech*, and
write the speech into the output buffer whenever it's ready.

```sh
python3 fxs/tools/audiosocket_demo.py --listen 0.0.0.0:9092
# on the router, once:  APP=<this PC's LAN IP>:9092 slic-asterisk-setup --no-apk
# then dial 800 # on the handset
```

Verified 2026-10-11: dialled from the handset, the PC heard and recorded
6.2 s of speech, and the caller heard the beep and their own voice.

---

## 9. Pitfalls found on the way

| Trap | Symptom | Fix |
|---|---|---|
| BusyBox `pgrep -x asterisk` finds nothing, though `/proc/PID/comm` is `asterisk` | every handset call went straight to busy tone ("asterisk not running") | `pidof asterisk` |
| `ExecIf` lives in `app_exec`, not installed | `No application 'ExecIf'`: the hangup-while-ringing cleanup never ran, the bell rang on to 45 s | `GotoIf` (built in) |
| `FILE()` writing to a FIFO | "not in line format": line mode *reads* the file first, which a write-only FIFO can't do | `System(echo … > fifo)` with `FILTER()`ed input |
| `FILE()` on `/proc/sys/kernel/random/uuid` | returns empty (proc files report size 0) | `SHELL(cat …)` |
| `SPRINTF(%c,10)` for a newline | yields nothing | `echo` adds the newline |
| `;` inside a call file | starts a comment and silently truncates the value | no `;` in call-file values |
| pjsip endpoint `acl=lan-only` | "invalid ACL": an endpoint's `acl=` names an `acl.conf` entry, not a pjsip one | a global `type=acl` section in `pjsip.conf` only |
| bundled sounds are GSM | `Playback` fails: no translation gsm → ulaw | `asterisk-codec-gsm` |
| `channel originate` from the CLI | unknown command: `res_clioriginate` isn't packaged | call files (`pbx_spool`) |
| BusyBox has no `base64`, `od`, `install`, `timeout`; `sleep` takes whole seconds | the first secrets generator wrote empty passwords (caught before use) | `tr -dc 'A-Za-z0-9' < /dev/urandom \| head -c 20` plus a length check; `cp` + `chmod` |
| "There are no local system nameservers configured" at startup | looks alarming | harmless: pjsip falls back to the system resolver |
| "The canary is no more" in the log | looks like a crash | harmless: Asterisk lowered its own scheduling priority |

---

## 10. Later: an SLT SIP trunk

The ISP's (SLT's) voice service would arrive as a SIP account on VLAN 10, the
`voip_br` interface. **Not started, deliberately:** `voip_br` stays disabled
while the router carries the household internet, and doc 07 found that
VLAN 10 gives no DHCP offer without SLT provisioning. When it's time, the
work is:

1. SLT's SIP credentials (registrar, username, password); the stock Tozed
   firmware's SIP fields (doc 09 §3.3) show what it expects.
2. Bring up VLAN 10 (`voip_br`) with an address, *without* touching the
   pppoe default route.
3. A pjsip `registration` + `endpoint` for the trunk, with a transport bound
   to the VLAN 10 address, and an ACL permitting only SLT's servers.
4. Dialplan: an outbound prefix (e.g. `9` + number) → trunk; inbound calls
   on the trunk → `100` (and/or a SIP extension or an app).
5. Firewall: a zone for VLAN 10 that admits SIP and RTP only from SLT.

Everything above it (extension 100, the apps) stays as is: to them, the
trunk is just another place a call can come from.

---

## 11. Open items

| Item | Why it matters | Next step |
|---|---|---|
| Echo cancellation | the far end (softphone, app) hears itself ~20 dB down; an AI agent may mistake it for the caller interrupting | built (OSLEC in the PCM driver, kmod r3, §7.1): verified on the cradle, 33 dB / ~44 dB; off-hook measurement and a listening test still to do |
| Handset keypresses during a call | apps on 800 get them only as tones | detect in the app, or have the daemon translate them into a dialplan action |
| Long-call clock drift | the 100 ms cap would start dropping frames | log `frames dropped` on a 10-minute call |
| Hook flash | could give transfer / call waiting | not handled; the daemon sees it as a short on-hook |
| Bell cadence | the phone rings 2 s on / 4 s off (US style); Sri Lanka uses a double ring | change the daemon's cadence, re-test caller ID timing |

## How obtained

All on the router (OpenWrt 25.12.5, Asterisk 23.1.0-r1 from the telephony
feed) on 2026-10-11. Protocol details (frame kinds, the 2 s timeout, DTMF
direction, `chan_audiosocket` answering on connect) were read from the
Asterisk 23.1.0 source (`res/res_audiosocket.c`,
`apps/app_audiosocket.c`, `channels/chan_audiosocket.c`). Call timings and
outcomes come from `logread -e slicctl` and
`/var/log/asterisk/messages.log` during live tests with a Uniden AS7412 on
the RJ11 jack; resource figures from `/proc/PID/status`, `free`, `netstat`;
audio levels from the 800 test's recording.
