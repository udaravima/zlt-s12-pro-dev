# fxs — RJ11 phone port for the ZLT S12 Pro on OpenWrt

Brings the router's analog phone jack (Microchip Le9641 SLIC on the MT7621)
back to life on OpenWrt 25.12.5: ring, dial tone, hook detection, audio clip
playback, caller ID, keypad dialling, and calls through an Asterisk PBX on
the router (the phone becomes extension 100). **Read [docs/09-phone-port-fxs.md](../docs/09-phone-port-fxs.md)
first**: it explains how the hardware works, how this was worked out, and the
safety notes. The architecture/onboarding guide is
[10-phone-port-architecture.md](../docs/10-phone-port-architecture.md);
the PBX is [11-pbx-asterisk.md](../docs/11-pbx-asterisk.md)
(hands-on guide: [12-pbx-setup-guide.md](../docs/12-pbx-setup-guide.md)).

| Path | What |
|---|---|
| `kmod-slic-mt7621/` | OpenWrt kernel package: `slic_spi_cs1` (SLIC on SPI CS1 → `/dev/spidev0.1`), `mt7621_pcm_raw` (PCM clock + `/dev/slicpcm`) |
| `slicctl/` | SLIC control tool + `daemon` mode on Microsemi VP-API-II (`fetch-vpapi.sh` pulls the API) |
| `slic-fxs/` | OpenWrt package: slicctl, `/etc/init.d/slic` (late load + boot-loop failsafe), `/etc/config/slic`, slic-play |
| `slicctl/cid.c` | Caller-ID generator: MDMF + Bell 202 / V.23 FSK, DTMF |
| `slicctl/dtmf.c` | Keypad (DTMF) detector on the handset audio |
| `slicctl/asock.c` | AudioSocket framing: the daemon's link to Asterisk |
| `slic-fxs/files/slic-dial` | Dial hook: turns dialled digits into an Asterisk call file |
| `asterisk/` | LAN-only PBX templates (`pjsip.conf`, `extensions.conf`, `rtp.conf`, `indications-lk.conf`) and `slic-asterisk-setup` |
| `tools/dtmf_test.py` | Offline DTMF detector test suite (`python3 tools/dtmf_test.py <host slicctl>`) |
| `tools/audiosocket_demo.py` | Template AudioSocket app for extension 800 (beep + parrot, saves a .wav) |
| `tools/sip_bot.py` | Template SIP application, stdlib only: logs in as an account, answers (beep + parrot, records, prints keys) or calls a number and plays a WAV |
| `tools/extract_profiles.py` | Rebuilds `slicctl/stock_profiles.c` from a stock-firmware dump |
| `tools/cid_decode.py` | Independent caller-ID decoder for generated files and line captures |
| `tools/slic-play.sh` | Installed as `slic-play`: play `.s16` clips through the daemon |
| `tools/gen_test_audio.py`, `tools/pcm_analyze.py` | Test audio; capture analysis (frequency, purity) |
| `reference/` | Decompiled stock Tozed device tree |
| `build.sh` | Builds the three `.apk` packages (and bare `.ko`s) into `build/` with the OpenWrt SDK |

Quick start:

```sh
export SDK=/path/to/openwrt-sdk-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64
./slicctl/fetch-vpapi.sh
python3 tools/extract_profiles.py /path/to/stock-mtd4.bin slicctl/stock_profiles.c
./build.sh                     # ~10 min: also repackages all SDK kmods
scp -O build/*.apk root@ROUTER:/tmp/
ssh root@ROUTER 'apk add --allow-untrusted /tmp/kmod-slic-spi-cs1-*.apk /tmp/kmod-mt7621-pcm-raw-*.apk /tmp/slic-fxs-*.apk'
```

Then: lift the handset for dial tone, `echo "ring 15 0123456789 NAME" > /var/run/slic.ctl` (ring with caller ID), `slic-play /usr/share/slic/scale_c4_c5.s16`.

PBX: `apk update && slic-asterisk-setup` on the router (installs Asterisk,
LAN-only config, prints the SIP accounts). Dial `601` or `600` on the handset;
`APP=host:port slic-asterisk-setup --no-apk` points extension 800 at your app.

As an OpenWrt feed instead (other owners): add
`src-git zltphone https://github.com/udaravima/zlt-s12-pro-dev.git`
to the SDK's `feeds.conf`, then `./scripts/feeds update zltphone && ./scripts/feeds
install -p zltphone slic-fxs`. Full steps in doc 09 §12.

Not committed on purpose: the Microsemi API (© Microsemi, no redistribution
licence) and the Tozed profile bytes (stock firmware data).
