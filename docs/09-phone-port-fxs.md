# 09 — The RJ11 phone port: from dead jack to ringing phone

Written 2026-10-08. Router: ZLT S12 Pro (Tozed board name
`s12-unisoc-plus`), OpenWrt 25.12.5, kernel 6.12.94. Code: [`fxs/`](../fxs).

## TL;DR

The "Phone/Line" RJ11 jack is a real analog phone port (an **FXS** port). It's
driven by a **Microchip (ex-Microsemi/Zarlink) Le9641 SLIC** that hangs off the
**router's MT7621 SoC**, not off the modem. Doc 07 searched for it on the
modem side, which is why it wasn't found there. Stock Tozed firmware drove it
with closed userspace and kernel modules that don't exist on OpenWrt, so on
OpenWrt the jack was electrically dead.

We brought it back without reflashing anything. Two small kernel modules plus
a userspace control tool run Microsemi's own SLIC library.

| Capability | Status | How verified |
|---|---|---|
| Talk to the SLIC over SPI | ✅ works | Chip ID read back: `PCN=0x49` = Le9641, revision 8 |
| Initialise + calibrate the line with Tozed's settings | ✅ works | API reports init and calibration complete, no fault flags |
| **Ring the phone** | ✅ works | Owner heard it ring |
| Built-in dial tone in the handset | ✅ works | Owner heard the tone |
| Hook detection (lift / hang up) | ✅ works | Every lift and hang-up reported, incl. a 1.5 s flash |
| **Play an audio clip** to the handset | ✅ works | Loopback capture shows a clean 1 kHz tone at −19.5 dBFS, and the owner heard the 1 kHz tone and the scale correctly |
| Record audio from the handset microphone | ✅ works | Speech recorded during live calls at −28 to −36 dBFS (doc 11 §7) |
| Keypad dialling (DTMF) | ✅ works | Software detector in the daemon; every digit of `601`, `600#`, `800#` detected live (doc 11 §6) |
| Calls through Asterisk (phone = extension 100) | ✅ works, LAN only | Dial out and be called with caller ID and two-way audio, 2026-10-11 (doc 11) |
| SIP trunk to the ISP (SLT) | ❌ not started | Deliberately deferred: the router carries the household internet; `voip_br` stays disabled |
| Installed as packages, starts at boot | ✅ works, survives reboot | `slic-fxs` + 2 kmod packages; service enabled (`S99slic`); daemon runs the line (§10) |
| Ring → answer → play clip → dial tone, via the service | ✅ works | Owner picked up after 10 s; daemon log shows ring → talk → playback → dial tone → idle |
| **Caller ID (Bellcore FSK, MDMF)** | ✅ works | Number shown on a Uniden AS7412 (Asia model) after the first ring; also decoded from a line loopback capture (§14) |
| Caller ID, V.23 FSK / DTMF variants | 🟡 implemented, not confirmed | Generated and decoded correctly offline; loopback decode failed/garbled; not tried on the phone |

---

## 1. What an FXS port actually needs

An old-fashioned phone is dumb. Everything that makes it a phone comes from
the line:

- **Battery**: about −48 V DC so the phone has power and the exchange can see
  it (loop current flows when you lift the handset, which is how "off-hook"
  is detected).
- **Ringing**: about 50–65 V AC at 20–25 Hz on top of the battery. That's what
  rings the bell.
- **Audio**: analog voice both ways on the same two wires.

A **SLIC** (Subscriber Line Interface Circuit) is the chip that does all of
this. The textbook acronym is BORSCHT: Battery, Overvoltage protection,
Ringing, Supervision (hook detection), Codec, Hybrid (2-wire↔4-wire), Testing.
The Le9641 is a modern single-chip SLIC that also generates its own
high-voltage supply with an on-board switching regulator. A SLIC talks to the
host computer over two separate digital buses:

```
            control: SPI (registers, commands, events)
  host ─────────────────────────────────────────────── SLIC ── RJ11 ── phone
   SoC      audio:   PCM (a 2.048 MHz bit clock, an 8 kHz frame
                     sync, and one 16-bit sample per direction per frame)
```

**The trap we hit:** the PCM clock isn't only for audio. This chip family
runs its internal logic from a PLL locked to the PCM clock. **With no PCM
clock, the chip can't even be initialised.** Register reads look fine, but the
library's sanity checks fail. You need the audio clock before you can ring.

---

## 2. The hardware, as found

```
                     SPI0 chip-select 1, SPI mode 0, 1 MHz (control)
  MT7621 (router) ────────────────────────────────────── Le9641 SLIC ── RJ11
     │   PCM pins: GPIO 9–12 (pin group "uart2" in "pcm" mode)   ▲
     │                                                            │
     │                       GPIO 32 "slic_clk_switch" ───────────┤ selects the SLIC's
     │                       0 = MT7621 PCM (SIP mode)            │ PCM clock/bus source
     │                       1 = modem PCM  (VoLTE mode)          │
     └── USB ── Unisoc SL8563 LTE modem ── its own PCM/I2S ───────┘
```

| Item | Value | Where it came from |
|---|---|---|
| SLIC | **Le9641PQC**: 1 line, "tracker" supply, up to 65 Vrms ring at 5 REN | Chip ID `0x49` read live; owner identified the part |
| SLIC supply topology | **Buck-boost tracker** | Decoded from Tozed's device profile (§4.3) |
| Control bus | SPI0 **CS1**; the boot NOR flash is CS0 on the same bus | Stock device tree: `spidev@1` |
| Audio bus | MT7621 PCM controller at `0x1E002000`, pins GPIO 9–12 | Stock device tree: `pinctrl0 { pcm { group = "uart2"; function = "pcm"; } }` |
| Clock switch | GPIO 32 (`gpiochip1` line 0), named `slic_clk_switch` | Stock device tree (disguised as an LED); behaviour measured (§5.3) |
| Phone LED | GPIO 31 (`blue:phone`) | Stock and OpenWrt device tree |

---

## 3. How we found all this (the investigation)

### 3.1 Not on the modem

The SL8563 modem runs its own Linux (`adb shell` from the router gives root).
It has sound cards and a voice daemon, `audiocmp`, but no SLIC. Its only SPI
device with an interrupt, `spi5.0`, turned out to be the power-management
chip (SC2721). That's what doc 07 recorded. The vendor datasheet for the
identical "JT3200U" board says "SIP VoLTE CPE … 1 RJ11
LINE port … UNISOC SL8563", which confirmed the port was real and meant for
voice.

### 3.2 Firmware archaeology on the stock image

We had a dump of the original Tozed firmware (`OpenWrt.mtd4.firmware.bin`,
16 MB). Its root filesystem (squashfs) is **partly corrupt**:
`unsquashfs` fails with "read_id_table", and binwalk only recovers ~30 blocks.

**The trick that worked:** squashfs compresses each data block as an
independent xz stream. So instead of mounting the filesystem, search the image
for every xz magic (`FD 37 7A 58 5A 00`) and decompress each one on its own.
That recovered **190 of 191 blocks (37 MB)**. Small files (under 256 KB) come
out whole. That's how we got the `mislic` binary intact, plus most scripts and
strings. This decoding is built into `fxs/tools/extract_profiles.py`.

What the stock firmware contained:

| Piece | What it does |
|---|---|
| `slic_spidev` | exposes the SLIC as `/dev/spidev0.1` |
| `kmod-telkits-pcm-driver` | MT7621 PCM driver (`pcmt_*` functions, DMA, a char device) |
| `mislic` | the line daemon, built on **Microsemi VP-API-II, `vp886` family**. Its strings name `api_lib/vp886_api/*.c` and "ZL880 or miSLIC families" |
| `tz_phone` | the call manager: VoLTE via modem AT commands, or SIP via its own `srsip` UA |
| `libosip2`, `siproxd` | SIP support libraries |

The **stock kernel's device tree** was the other key. The kernel is an
LZMA-compressed uImage at offset 0. After decompressing it, the device tree
(magic `d00dfeed`, 10,720 bytes) sits at offset `0x6b1590` and decompiles
with `dtc`. It's saved as `fxs/reference/stock_tozed_s12-unisoc-plus.dts`. It
gave the SPI chip-select, the PCM pin mux, and the `slic_clk_switch` GPIO.

### 3.3 The two voice modes in the stock firmware

The stock `tz_phone` init script chose the mode automatically:

```sh
# if any SIP account field is empty -> VoLTE through the modem, else -> SIP
[ -z sip_reg_account -o -z sip_reg_pwd -o -z sip_reg_server -o -z sip_reg_port ] &&
    tz_voice_type=0  volte_enable=1      # VoLTE: modem places the call
||  tz_voice_type=1  volte_enable=0      # SIP: srsip on the router
```

`tz_phone` also switched the clock like this: stop `mislic` → write GPIO 32
→ start `mislic`. At the time we didn't know why. §5.3 shows it moves the SLIC
between the modem's audio bus and the router's.

### 3.4 Why VoLTE through the modem wouldn't help here anyway

The modem keeps a VoLTE operator allow-list in `/etc/volte-conf.xml` (186
entries). For Sri Lanka (MCC 413) it lists only **413-02**. The Mobitel SIM in
this router is **413-01**, which isn't on it. So the VoLTE mode would refuse
calls even on stock firmware. The router-side (SIP) path is the useful one.

---

## 4. What we built

All code is in [`fxs/`](../fxs). It needs the official OpenWrt SDK for
**25.12.5 ramips/mt7621**: kernel modules must match the running kernel's
"vermagic" string exactly (`6.12.94 SMP mod_unload MIPS32_R2 32BIT`), and the
SDK builds against the exact same kernel.

### 4.1 `slic_spi_cs1.ko`: making the SLIC visible

OpenWrt's device tree for this router only declares the flash on SPI. The
proper fix would be a device-tree change, which means rebuilding and
reflashing the firmware. Instead, this tiny module finds the running SPI
controller and **registers a device on chip-select 1 at runtime**. It uses the
name `dh2228fv`, which happens to be both what Tozed used and an entry in the
kernel's `spidev` driver table. The stock `spidev` driver then binds and
`/dev/spidev0.1` appears.

**The trap:** the stock firmware used SPI **mode 3**, but OpenWrt's MT7621 SPI
driver forces **mode 0** on every transfer, because other modes garble bits
on this SoC. Mode 0 and mode 3 both sample data on the same (rising) edge, so
it was likely but not certain to work. The first ID read settled it:
`RCN=0x08 PCN=0x49`, a clean Le9641.

### 4.2 `slicctl`: running Microsemi's library in userspace

Driving a SLIC by hand means hundreds of registers and calibration state
machines. Microsemi's **VP-API-II** library does all of that. A copy of
version 2.24 "lite" with the `vp886` code is bundled in the GPL project
[pgid69/bcm63xx-phone](https://github.com/pgid69/bcm63xx-phone) (a SLIC driver
for old Broadcom routers). It also includes Microsemi's API manual for this
exact chip family (`VP_Api_ZL880-miSLIC_Rev8.pdf`).

`slicctl` is a small command-line program around that library:

| Part | What it does |
|---|---|
| `arch/vp_hal.c` | The only hardware access: turns the library's "MPI command" calls into spidev transfers (`[0x4A ec]` channel select, command byte, data; odd command = read) |
| `arch/sys_service.c` | Critical sections and delays the library needs (single-threaded, so trivial) |
| `slicctl.c` | Commands: `id`, `clk`, `ring`, `tone`, `monitor`, `talk` |
| `stock_profiles.c` | Tozed's settings, extracted from `mislic` (generated, not committed) |

Configuration changes to the library (applied by `fetch-vpapi.sh`): enable the
886/887 family instead of the bcm63xx's 880, and use **simple polled** mode,
because we don't know which GPIO, if any, the SLIC's interrupt pin reaches.
`slicctl` polls the chip every 10 ms instead.

### 4.3 Why Tozed's profiles were mandatory

The library is configured with "profiles": byte arrays from Microsemi's
Profile Wizard that set the PCM clock, the switching-regulator design, line
voltages, ringing waveform and so on. The SLIC drives its **own high-voltage
switching regulator**, so a profile written for a different supply design
could stress or damage the hardware.

Tozed's device profile, found inside `mislic` at offset `0x27810`, decoded:

```
0D FF 00 33 08 | 14 | 46 02 | 44 00 | 5E 14 00 | F6 95 00 58 30 5C 30 | E4 04 8F 0A | E6 60 | 02 A0 ...
886/887 device  MPI  PCLK    slots   dev mode   switcher timing         switcher       ctrl   formatted:
format v8      len  2.048M  TS 0                                        params                A0 -> switcher = buck-boost
```

The supply type is **buck-boost**. Microsemi's sample profiles in the
bcm63xx-phone repo are for flyback and inverting-boost designs, so **neither
would have been correct**. We use Tozed's bytes verbatim. The library (API
2.24) accepted this newer format v8: it reads fields in order and ignores the
newer trailing fields.

### 4.4 `mt7621_pcm_raw.ko`: the audio clock and sample pipe

OpenWrt has no driver for the MT7621's PCM block. The register map came from
a [2020 openwrt-devel patch](https://lists.openwrt.org/pipermail/openwrt-devel/2020-July/029896.html)
(Puyou Lu, an ASoC driver tested on MT7621, never merged). Our module is
deliberately simpler: no ALSA, no DMA.

1. Enable the PCM clock gate and pulse the PCM block's reset.
2. **Program the analog audio PLL** (`ANA_CTRL` at `0x1E000F00`), exactly as
   the 2020 driver does (§5.4 explains why this is required).
3. Switch GPIO 9–12 to the PCM function (`GPIO_MODE` bits 6:5 = 2).
4. Make the MT7621 the bus master: bit clock = 294 MHz / 2 / 71.78 ≈
   **2.048 MHz**, 32 slots per frame → **8 kHz frame sync**. That's what the
   stock device profile tells the SLIC to expect.
5. Every 250 µs, an hrtimer refills the transmit FIFO from `/dev/slicpcm`
   (silence if there's nothing to play) and drains the receive FIFO.

Interfaces: `/dev/slicpcm` (write 16-bit signed little-endian mono 8 kHz to
play; read to record) and `/sys/kernel/debug/slicpcm/regs` (register dump,
counters, and a write interface for poking registers live).

---

## 5. What the hardware told us (experiments)

### 5.1 First contact

```
$ ./slicctl id
RCN=0x08 PCN=0x49  -> Le9641 (SF1 low-voltage 1ch tracker). SPI framing OK.
```

The chip accepts both SPI framings (chip-select toggled per byte, or held for
the whole command). We default to held: per-byte mode makes each 1-byte
transfer take a cache-line-sized slot in spidev's 4 KB buffer, and the
library sends bursts of up to 258 bytes. That fails with "Message too large".

### 5.2 Init fails with a clock fault, and that's a good failure

The first full init came back with "init complete" but data `0x0003` =
failure + **CFAIL (clock fault)**. Reading the library source showed it checks
the clock *first* and stops before starting the switching regulator. So no
line voltage was ever generated. **Note the trap:** the library reports
failures through the same "init complete" event; only its data word tells you
it failed.

### 5.3 What `slic_clk_switch` (GPIO 32) does

The quick register probe for CFAIL turned out unreliable: the bit stays
clear once the chip has locked once. So we used full init, which resets the
chip and re-checks the clock, as the test:

| GPIO 32 | MT7621 PCM driver | Full init |
|---|---|---|
| 1 | loaded | success |
| 1 | **unloaded** | **success**, so the clock must come from the **modem** |
| 0 | loaded, no PLL | fails (`ERR_SPI`: no usable clock at the SLIC) |
| 0 | loaded **with audio PLL** | **success**: clocked by the router |

**Conclusion:** GPIO 32 = 1 hands the SLIC's PCM bus to the modem (Tozed's
VoLTE mode); 0 hands it to the MT7621 (SIP mode). Ringing and tones only need
*a* clock, so they work in either position. Playing our own audio needs 0.

### 5.4 Why the audio PLL is required

Without the PLL step, the MT7621 PCM block runs (its FIFO drains), but no
usable clock reaches the SLIC. The PLL registers before and after the setup
code:

| Register | Before (as booted) | After |
|---|---|---|
| `ANA+0x00` | `0x00008800` | `0x00008c00` |
| `ANA+0x14` | `0x01001d61` | `0x01401d61` |
| `ANA+0x1c` | `0x80100004` | `0x001201ed` |

Bit 31 of `ANA+0x1c` is set at boot and the setup clears it. That looks like a
PLL power-down/disable bit, which would explain the dead clock. This is an
inference: we haven't found documentation for the bit. The crystal-mode field
in `SYSCFG0` (`0x002e22e2`, field = 3) selects the 20/40 MHz-crystal PLL values.
**Network impact: none observed.** Internet ping, both Wi-Fi radios and the
USB devices were all checked right after the PLL setup.

### 5.5 Two samples per FIFO word

Measured with nothing playing: the transmit FIFO (8 words deep) drains at
**~4,040 words/second**. The frame rate is 8,000/s, so each 32-bit FIFO word
carries **two 16-bit samples**. The first driver version put one sample per
word, which would have played everything an octave high at double speed.

### 5.6 Tuning without a listener: the loopback trick

With the line in the talk state, audio sent to the phone partly echoes back
through the SLIC's 2-wire/4-wire hybrid onto the receive side of the PCM bus,
where the driver captures it. So we can test the whole audio path **from the
router alone**, then analyse the captures on a PC
(`fxs/tools/pcm_analyze.py`: dominant frequency and "purity", the share of
energy in the main tone).

| Capture | Peak | Purity | Meaning |
|---|---|---|---|
| SLIC's own dial tone, samples as read | 450 Hz | 88% | right tone, plus a mirror image at 3,550 Hz |
| same, sample pairs swapped | **450 Hz** | **100%** | the first sample is in the **low** 16 bits of each word |
| same, bytes swapped | noise | 6% | byte order was already right |
| Our 1 kHz file played to the line (fixed packing) | **1000 Hz** | **100%** | playback works end to end, −19.5 dBFS echo |

So the bit alignment and timeslot were right from the start; only the packing
order needed flipping (`pack_hi_first=0`, now the driver default). The dial tone being 450 Hz also
confirms the decode of Tozed's tone profile.

### Status of the listening test

The full playback test (5 s at 1 kHz, then a C-major scale twice) ran with no
underruns (`tx_unrun 0`) while the owner held the handset. **Owner verdict:
it worked**: the tone and the scale played correctly. Confirmed again through
the installed service (§10).

---

## 6. Pitfalls, in one place

| Trap | Symptom | Fix |
|---|---|---|
| Kernel module builds use `-Werror` | an unused variable fails the build, and a careless copy deploys the *previous* `.ko` | `fxs/build.sh` refuses missing or stale modules |
| `CFLAGS=… make` on the command line | wipes the Makefile's include paths | Makefile uses `override CFLAGS +=` |
| Host GCC defaults to C23 | `bool` typedef in Microsemi headers fails | `-std=gnu11` |
| Library config `#undef`s the 886 family | build picks the 880 family | `fetch-vpapi.sh` edits `vp_api_cfg.h` |
| Library sends 258-byte bursts | HAL buffer overflow (segfault), then "Message too large" | 260-byte buffer, held-CS framing |
| "Init complete" event ≠ success | looks fine, nothing works | check the event data word: `0x0000` = success |
| PCM source clock | clk framework says 270 MHz; the audio PLL gives ~294 MHz | `setup_pll=1`, `freqin=294000000` (module defaults) |
| `pkill -f pattern` in a shell | kills the shell running it | match the process another way |
| Router busybox | no `timeout`, `sleep` takes whole seconds | avoid both |
| `./scripts/feeds update -i` on a `src-link` feed | only re-indexes; the `feeds/<name>` link keeps pointing at the **old** checkout, so the SDK silently builds stale sources | `build.sh` runs a full `feeds update` and aborts unless `feeds/zltphone` resolves to its own checkout |
| `./build.sh \| grep …` | the pipeline reports grep's exit status, so a failed build looks successful | run `build.sh` on its own and check `$?`; read `build/*-build.log` |
| Replacing `/usr/sbin/slicctl` while the daemon runs | `Text file busy` | `/etc/init.d/slic stop`, copy, start (or install the package) |
| A hand-copied test binary | the package database and the file disagree; the next upgrade silently replaces it | test with hand copies, but ship by installing the rebuilt package |
| SLIC timestamp-rollover event | logged every ~32 s, drowning real events in syslog | the daemon ignores `VP_DEV_EVID_TS_ROLLOVER` |
| Python `open(p,'w').write(f(open(p).read()))` | `open(p,'w')` runs first and truncates the file before it's read | read into a variable, then write (bit the public exporter once) |

---

## 7. How to reproduce

### 7.1 Build (on a Linux PC)

```sh
# once: OpenWrt SDK (225 MB) — verify against the release sha256sums
#   https://downloads.openwrt.org/releases/25.12.5/targets/ramips/mt7621/
#   openwrt-sdk-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst
export SDK=~/owrt-sdk/openwrt-sdk-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64

cd fxs
./slicctl/fetch-vpapi.sh                                   # Microsemi API, pinned commit
python3 tools/extract_profiles.py <stock mtd4 dump or mislic> slicctl/stock_profiles.c
./build.sh                                                 # -> fxs/build/
tar c -C build . | ssh root@ROUTER 'mkdir -p /tmp/slic; tar x -C /tmp/slic'
```

The profile file needs the stock firmware. Dump `mtd4` from a router
still on stock firmware, before flashing OpenWrt (see doc 06 for dumping).

### 7.2 Run (on the router)

```sh
apk add kmod-spi-dev gpiod-tools          # official packages, persistent
cd /tmp/slic
insmod ./slic_spi_cs1.ko max_hz=1000000   # -> /dev/spidev0.1
./slicctl id                              # expect PCN=0x49

# ring / dial tone: any clock works; the modem's is simplest
gpioset -c gpiochip1 -z 0=1               # GPIO 32 = 1: modem clock
./slicctl ring 5                          # rings for 5 s
./slicctl tone 30                         # lift the handset: 450 Hz dial tone
./slicctl monitor 60                      # hook events; tone while off-hook
killall gpioset

# play audio clips: router clock + PCM driver
./slic-play.sh tone_1khz_5s.s16 scale_c4_c5.s16
```

Your own clip: convert on a PC with
`ffmpeg -i clip.mp3 -ac 1 -ar 8000 -f s16le clip.s16`, copy it to the router,
then `./slic-play.sh clip.s16`. Pick up the handset when it says `TALK`.

### 7.3 Debug

```sh
cat /sys/kernel/debug/slicpcm/regs            # PCM registers, PLL, counters
echo "20 00000002" > /sys/kernel/debug/slicpcm/regs   # poke a PCM register (hex offset value)
SLIC_TRACE=1 ./slicctl id                     # log every SPI command
head -c 32000 /dev/slicpcm > cap.raw          # 2 s capture (s16le 8 kHz)
python3 fxs/tools/pcm_analyze.py cap.raw      # on the PC
```

Module parameters (`modinfo mt7621_pcm_raw.ko`): `setup_pll` (default 1),
`freqin` (294000000), `bclk_hz` (2048000), `slot_mode` (3 = 32 slots),
`ts_start` (1), `pack` (1), `pack_hi_first` (0, loopback-verified), `tick_us` (250).

---

## 8. Safety notes

- The line carries **up to ~100 V** when active or ringing. Don't touch the
  RJ11 contacts with the line powered.
- Only use profiles extracted from this board's stock firmware. The SLIC
  drives its own switching regulator.
- The SPI flash shares the bus (CS0). `slic_spi_cs1` only adds a CS1 device;
  it never touches CS0 settings.
- The PLL step writes analog registers. No side effects were seen, but a
  reboot restores them if anything ever misbehaves.
- Nothing here is persistent: a reboot returns the router to plain OpenWrt
  (only the `kmod-spi-dev`/`gpiod-tools` packages stay installed).

---

## 9. What's left

1. ~~Persistence~~, ~~a long-running daemon~~, ~~reboot test~~ (§10), ~~caller
   ID~~ (§14), ~~DTMF dialling~~ (doc 11 §6): done. Remaining: confirming the
   V.23 and DTMF caller-ID variants on a phone.
2. ~~**SIP**~~: done as a LAN-only Asterisk PBX with the phone as extension
   100 (**doc 11**). Remaining there: echo cancellation, and the ISP SIP
   trunk (doc 11 §10), still deferred while the router carries the home
   internet.
3. Optional: replace the PIO timer with DMA (GDMA request lines 6/5) or a
   proper ALSA driver, so standard audio tools work.

---

## 10. Running it permanently (packages + service)

After the bring-up, everything was packaged so the port works without manual
steps. Three OpenWrt packages, all built by `fxs/build.sh`:

| Package | Contents |
|---|---|
| `kmod-slic-spi-cs1` | `slic_spi_cs1.ko` (depends on `kmod-spi-dev`) |
| `kmod-mt7621-pcm-raw` | `mt7621_pcm_raw.ko`, which now also **claims GPIO 32 itself** (`clk_switch` parameter), so no `gpioset` is needed |
| `slic-fxs` | `/usr/sbin/slicctl`, `/usr/sbin/slic-play`, `/etc/init.d/slic`, `/etc/config/slic`, test clips in `/usr/share/slic/`; since release 2 also the Asterisk link: `/usr/libexec/slic-dial`, `/usr/sbin/slic-asterisk-setup`, config templates in `/usr/share/slic/asterisk/` (doc 11) |

### The boot-loop trap, and the failsafe

This router's kernel runs with `panic_on_oops=1` and `panic=3`: any kernel
crash reboots it 3 seconds later. Kernel modules are normally autoloaded from
`/etc/modules.d` early in boot. If one of ours ever crashed there, the router
would crash, reboot, load it again and crash again: a boot loop with no
internet. So the packages deliberately **don't autoload**. Instead:

1. `/etc/init.d/slic` runs at `START=99`, after networking is up.
2. Before loading the modules it writes `/etc/slic.loading` (on flash, synced).
3. When both modules have loaded, it deletes the marker.
4. If a boot ever finds the marker still present, the previous load never
   finished (it crashed the box), so it **skips the modules**, logs
   why, and the router comes up normally. To retry:
   `rm /etc/slic.loading && /etc/init.d/slic start`.

Worst case: one crashed boot, then the next boot is clean.

### What the daemon does

`slicctl daemon` initialises the SLIC once and keeps the line under control:

| Event / command | Result |
|---|---|
| handset lifted (idle) | line TALK + **dial tone**, phone LED on; keypad digits collected and dialled through Asterisk (doc 11 §5.3) |
| handset hung up | line STANDBY, LED off |
| `echo "ring 20" > /var/run/slic.ctl` | ring with 2 s on / 4 s off cadence for 20 s |
| handset lifted while ringing | ringing stops, line goes to TALK ("answered") |
| `echo talk > /var/run/slic.ctl` | TALK state: PCM audio reaches the handset, tone off |
| `echo release > /var/run/slic.ctl` | back to dial tone (if off-hook) or standby |
| `echo "ring 20 0771234567 NAME" > /var/run/slic.ctl` | ring **with caller ID** (number up to 20 digits, name up to 15 characters, no spaces) |
| `echo "cidfmt bell" > /var/run/slic.ctl` | caller-ID format until restart: `bell`, `v23` or `dtmf` (persistent default: uci `slic.main.cid_format`) |
| `echo "cidsend 0771234567 NAME" > /var/run/slic.ctl` | caller-ID burst only, no ringing (testing; many phones still show it) |

Asterisk uses two more commands, `call` and `hangup` (doc 10 §3.1, doc 11
§5). Current state: `cat /var/run/slic.state`
(`state=idle|dialtone|dialing|ringing|talk|busy`, `hook=on|off`). Logs: `logread -e slicctl`. `slic-play FILE.s16` wraps
`talk` → stream to `/dev/slicpcm` → `release`.

### Everyday commands

```sh
/etc/init.d/slic status | restart | stop
echo "ring 15" > /var/run/slic.ctl          # make the phone ring
slic-play /usr/share/slic/scale_c4_c5.s16   # play a clip (pick up first)
uci set slic.main.enabled=0; uci commit slic; /etc/init.d/slic stop   # disable
apk del slic-fxs kmod-mt7621-pcm-raw kmod-slic-spi-cs1                # remove completely
```

### Installing from a build

```sh
cd fxs && ./build.sh                                   # -> fxs/build/*.apk
scp -O build/*.apk root@ROUTER:/tmp/
ssh root@ROUTER 'apk add --allow-untrusted /tmp/kmod-slic-spi-cs1-*.apk \
    /tmp/kmod-mt7621-pcm-raw-*.apk /tmp/slic-fxs-*.apk'
```

`--allow-untrusted` is needed because these are locally built, unsigned
packages. Notes:
- The kmods are tied to this exact kernel build (`kernel=6.12.94~35fb64ea…`).
  **After a firmware upgrade they must be rebuilt** with the matching SDK.
- `kmod-mt7621-pcm-raw` r3 and later needs `kmod-echo` (the kernel's OSLEC
  echo canceller). It is an official package, so `apk add` fetches it from
  the feed (the router needs internet access, or `kmod-echo` installed
  beforehand).
- Building `slic-fxs` makes the SDK repackage all kernel modules (because of
  the `kmod-spi-dev` dependency), so one `build.sh` run takes ~10 minutes.

### Verified 2026-10-08

Installed with `apk`. The service started itself (`S99slic` enabled). The
module claimed GPIO 32 and the daemon initialised and calibrated the line.
Then an end-to-end run with the owner at the handset:
`ring 20` → picked up after 10 s → `talk` → 1 kHz + scale played → `release`
→ dial tone → hung up → `idle`. **Reboot test passed** (21:41): after the
owner rebooted, the service came back by itself (modules loaded, failsafe
marker absent, line `idle`, internet up), and ring and playback worked again.

---

## 11. What's in git and what isn't

| Committed (own work, GPL-2.0 for the kernel modules) | Kept out (fetched or generated locally) |
|---|---|
| `fxs/kmod-slic-mt7621/` both kernel modules + OpenWrt package Makefile | Microsemi VP-API-II and its headers: © Microsemi, no redistribution licence. `fetch-vpapi.sh` pulls them from bcm63xx-phone at commit `a49d39e` |
| `fxs/slicctl/` `slicctl.c`, `arch/vp_hal.c`, `arch/sys_service.c`, Makefile |
| `fxs/slic-fxs/` package Makefile, init script, uci config | `slicctl/stock_profiles.c`: Tozed firmware data, regenerated by `extract_profiles.py` |
| `fxs/tools/` extractor, analyser, test-audio generator, play script | build outputs, captures, `.ko` |
| `fxs/reference/stock_tozed_s12-unisoc-plus.dts` (decompiled stock DT) | |

---

## 12. Using this repo as an OpenWrt feed

Other owners of this router can build the packages with the standard OpenWrt
feed mechanism. In the 25.12.5 ramips/mt7621 SDK:

```sh
echo "src-git zltphone https://github.com/udaravima/zlt-s12-pro-dev.git" >> feeds.conf
./scripts/feeds update zltphone
./scripts/feeds install -p zltphone kmod-slic-spi-cs1 kmod-mt7621-pcm-raw slic-fxs

# the two inputs that can't be distributed:
(cd feeds/zltphone/fxs/slicctl && ./fetch-vpapi.sh)
python3 feeds/zltphone/fxs/tools/extract_profiles.py <your stock mtd4 dump> \
        feeds/zltphone/fxs/slicctl/stock_profiles.c

make menuconfig        # Utilities -> slic-fxs (=m), pulls in both kmods
make package/slic-fxs/compile
# packages land in bin/targets/ramips/mt7621/packages/ and bin/packages/.../zltphone/
```

The feed scanner finds exactly the three packages (`fxs/kmod-slic-mt7621`,
`fxs/slic-fxs`); verified 2026-10-08 with `src-link` pointing at this
checkout. `fxs/build.sh` uses the same mechanism (`src-link` to the local
checkout) and copies the `.apk`s to `fxs/build/`.

If the SDK was used with older direct `package/` symlinks, `feeds install`
refuses with "Not overriding core package"; delete those symlinks and
`tmp/`, then re-run.

---

## 13. Why this isn't in OpenWrt itself (and what it would take)

Everything above lives in this repo and runs as locally built packages.
OpenWrt's own device tree for this router declares only the SPI flash and the
phone LED. Upstreaming would need:

| Piece | Upstreamable as is? | Blocker / required work |
|---|---|---|
| `slicctl` | No | Built on Microsemi VP-API-II, which has no open licence; Tozed's profile bytes are firmware data. Needs an openly licensed Le9641 driver, but the full datasheet (V9, 84 pages) documents the chip only through VP-API-II functions, with **no register map**. **The real blocker.** |
| `slic_spi_cs1` | No | A runtime workaround for a missing DT node; DT reviewers reject bare spidev nodes, so it needs a real SLIC driver to bind to. |
| `mt7621_pcm_raw` | Not in this form | Bring-up quality (fixed addresses, hrtimer PIO, debugfs register poke). The acceptable form is an ASoC driver plus a DT node in `mt7621.dtsi`. The 2020 attempt stalled at review; our findings (audio PLL required, low-half-first packing, 2 samples per FIFO word) would help a revival. |
| DT additions (PCM pinmux, GPIO 32) | Trivial | Useless without the drivers. |

Realistic now: this feed (§12) plus a forum write-up. Longer term: the MT7621
PCM ASoC driver (achievable), then an open Le9641 driver (hard).

---

## 14. Caller ID: how it works, and how it's done here

### How analog caller ID works

The number travels as a short **data burst on the line while the phone is
still on-hook**. The phone's caller-ID chip listens during that window:

| Variant | Signal | When | Used in |
|---|---|---|---|
| **Bellcore / Telcordia GR-30** | FSK, 1200 baud, Bell 202 tones (mark 1200 Hz, space 2200 Hz) | **between the 1st and 2nd ring** | North America, much of Asia |
| **ETSI EN 300 659** | FSK, 1200 baud, V.23 tones (mark 1300 Hz, space 2100 Hz) | before the 1st ring (after a line-polarity reversal or a short ring pulse), or after the 1st ring | Europe, UK variant |
| **DTMF CID** | DTMF digits, e.g. `D` + number + `C` | before the 1st ring, usually after polarity reversal | India, parts of Scandinavia |

An FSK burst is: channel seizure (alternating 0/1 bits), a run of mark bits,
then a message. The **MDMF** format carries parameters: date/time
(`MMDDHHMM`), number, name, each as type-length-value, plus a checksum. Many
modern phones auto-detect more than one variant. Which one Sri Lankan phones
expect was unknown at the start. **Verified 2026-10-11: the owner's Uniden
AS7412 (Asia model) displays Bellcore FSK / MDMF.** The owner's manual for
the same family (AS7413, Singapore distributor) lists "DTMF/FSK Caller ID",
so the other variants may work too (not tested). The same manual gives the
phone's hook-flash options (90/95/100/120/180/300/600/1000 ms, default
600 ms), which matters once the daemon detects flash.

### What the hardware and stock firmware do

- The **Le9641 can generate caller ID itself**: the datasheet lists "Universal
  Caller ID generation (FSK and DTMF signaling)" and a Caller ID profile
  (§10.10, Table 22). The line must be in
  **on-hook transmission** (`VP_LINE_OHT`) so audio passes while the phone is
  idle.
- Stock `mislic` contains Microsemi's full caller-ID engine (FSK buffering,
  checksums, DTMF CID) in the sequencer module `vp886_seq.c`, and `tz_phone`
  has a `dtmf_type` setting that presumably picks the variant.
- **Our VP-API copy is the "lite" edition and lacks `vp886_seq.c`**, so the
  API's `VpInitCid`/`VpSendCid` path isn't available here.

### What we built: caller ID in software

The audio path into the line is already proven (§5.6), so caller ID is
**generated as 8 kHz audio and played through `/dev/slicpcm`** at the right
moment in the ring cycle, with the line in on-hook transmission. No
Microsemi sequencer needed.

**Pieces** (all in `fxs/slicctl/`):

| File | Role |
|---|---|
| `cid.c` / `cid.h` | Signal generator: MDMF message builder (date/time `MMDDHHMM` from the router clock, number, optional name, checksum), 1200-baud phase-continuous FSK (Bell 202 or V.23 tones), DTMF `D…C` |
| `slicctl.c` | `cidgen` command (writes a signal to stdout, no hardware); daemon `ring … NUMBER [NAME]`, `cidfmt`, `cidsend` |
| `../tools/cid_decode.py` | **Independent** decoder (different language and method: tone correlation + UART framing + checksum), for generated files and line captures |

**Signal details** (what a reader of `cid.c` needs to know):

- FSK runs at 1200 baud; at 8 kHz that's 6.667 samples per bit, so the
  generator carries the fractional remainder from bit to bit, and keeps the
  sine phase continuous across bits (a phase jump would splatter energy
  across the band).
- Frame: 300 bits channel seizure (`0101…`), 180 mark bits, then each byte
  as start bit `0`, 8 data bits LSB first, stop bit `1`, then 10 trailing
  marks. The Bellcore burst for a 10-digit number plus a 6-letter name is
  **0.68 s**.
- Level: 0.25 of full scale (about −12 dBFS), adjustable with the
  environment variable `SLIC_CID_LEVEL`.

**Timing in the daemon:**

| Format | Sequence |
|---|---|
| `bell` (default) | ring 2 s → **wait 0.6 s** (GR-30 asks for at least 0.5 s) → line `VP_LINE_OHT` → burst → +0.3 s margin → standby → the cadence continues (4 s off total, then ring) |
| `v23`, `dtmf` | line `VP_LINE_OHT_POLREV` (polarity reversal, the usual "caller ID coming" alert) → wait 0.3 s → burst → +0.3 s → standby with normal polarity → **then** the first ring |

`write()` to `/dev/slicpcm` returns once the samples are queued (the driver
buffers 0.5 s), so the daemon computes the burst's end time from its length
instead of waiting on the write.

### How it was verified

1. **Generator vs independent decoder, offline**:
   `slicctl cidgen bell 0771234567 UNIDEN > cid.s16`, then
   `cid_decode.py cid.s16 bell` gives `type 0x80 (MDMF) … checksum OK`, the
   date/time, `0771234567` and `UNIDEN`. V.23 and DTMF decode just as cleanly
   (DTMF: `D0771234567C`).
2. **Line loopback, no ringing**: while `cidsend` played the burst, the
   driver recorded the echo coming back from the line, and that recording
   was decoded on the PC:

   | Format | Loopback result |
   |---|---|
   | bell | ✅ full MDMF decoded, checksum OK, name `LOOPBACK` |
   | v23 | ❌ nothing decoded |
   | dtmf | 🟡 garbled (`1D077123425647C`) |

   The v23/dtmf runs use the reversed-polarity state. Whether their echo is
   simply weaker or distorted (a decoder problem) or the signal on the line
   is really damaged is **not established**.
3. **On the phone**: `ring 20 0771234567 UNIDEN` → the first ring, then the
   burst 0.6 s later (daemon log: `caller ID bell '0771234567' sent (0.68 s)`)
   → **the Uniden displayed the caller ID** (owner confirmed).
4. **Packaged release** (`slic-fxs 2026.10.11`, installed with `apk`):
   `cidsend 0771234567 PACKAGED` recorded off the line decodes to MDMF,
   checksum OK, with the router's **local** time (`10110930`), so the
   timezone reaches the phone's display.

### Applying it later

SIP (or any application) only has to pass the caller's number in the ring
command: `ring 30 <number> <name>`.

## References

- Le9641 product brief (Microsemi doc 150402) and full datasheet V9
  (PD-248191) on microchip.com. The full datasheet must be downloaded in a
  browser; the site blocks scripts.
- VP-API-II manuals, bundled in bcm63xx-phone:
  `VP_Api_ZL880-miSLIC_Rev8.pdf`, `VP_API_II_An_Applications_Perspective_Rev1.pdf`.
- MT7621 PCM register map: openwrt-devel, July 2020, "[PATCH 1/1] ramips:
  mt7621: add ralink pcm controller driver".
- Linux 6.12 sources consulted: `drivers/spi/spi-mt7621.c` (mode 0 forced),
  `drivers/spi/spidev.c` (id table), `drivers/pinctrl/mediatek/pinctrl-mt7621.c`
  (uart2 group: `pcm` = 2), `drivers/clk/ralink/clk-mt7621.c` (`pcm` gate =
  CLKCFG1 bit 11).

## How obtained

All hardware results above were gathered live on 2026-10-08 over
`/usr/bin/ssh root@192.168.1.254`, with the owner at the handset for the ring,
tone and hook tests. Firmware analysis used a dump of the stock firmware made before
flashing OpenWrt.
