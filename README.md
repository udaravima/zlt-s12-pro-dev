# ZLT S12 Pro: developer notes for OpenWrt

Hands-on research on the Tozed **ZLT S12 Pro** LTE router (board name
`s12-unisoc-plus`) running **OpenWrt 25.12.5**. Inside are two computers:

- a **MediaTek MT7621** router SoC running OpenWrt, and
- a **Unisoc SL8563** LTE modem on USB, running its **own Linux** (root shell
  via `adb` from the router).

These docs record what each part can really do, how it was found, and how to
reproduce it. Each document has a "How obtained" section with the commands.

## Highlights

| Topic | Result | Doc |
|---|---|---|
| **RJ11 phone port** | Dead on OpenWrt → **rings, dial tone, hook detection, audio playback**, packaged and boot-safe | [09](docs/09-phone-port-fxs.md), code in [`fxs/`](fxs) |
| Modem as a peer | Root shell, dedicated NCM link to the router (~6–19 MB/s), modem-side tooling | [03](docs/03-interconnect.md), [05](docs/05-expansion.md) |
| Full modem firmware dump | 128 MB NAND dumped over USB, md5-verified; method and pitfalls | [06](docs/06-fw-dump.md) |
| WWAN data link | NCM dial path fixed; metric-based failover between PPPoE and LTE | [08](docs/08-wwan-ncm-data.md) |
| Voice / SMS | AT channel, oFono on the modem, SIM and network behaviour | [07](docs/07-voice-sms.md) |

## Documents

| Doc | Contents |
|---|---|
| [README](docs/README.md) | Project timeline and current state |
| [01-router](docs/01-router.md) | Router hardware, flash layout, overlay pressure |
| [02-modem](docs/02-modem.md) | Modem: NAND/UBI layout, userland, USB gadget |
| [03-interconnect](docs/03-interconnect.md) | adb/ssh access chain, benchmarks, persistence |
| [04-design-options](docs/04-design-options.md) | Package-storage designs, dead ends, recommended `/opt` scheme |
| [05-expansion](docs/05-expansion.md) | Modem as a peer: RAM/boot limits, NCM link, throughput |
| [06-fw-dump](docs/06-fw-dump.md) | Full NAND dump: method, verification, pitfalls |
| [07-voice-sms](docs/07-voice-sms.md) | Voice/SMS: AT channel, oFono, SIM, network registration |
| [08-wwan-ncm-data](docs/08-wwan-ncm-data.md) | WWAN data link: ncm proto, dial mechanism, routing metrics |
| [09-phone-port-fxs](docs/09-phone-port-fxs.md) | RJ11 FXS port: Le9641 SLIC on the MT7621, kernel modules, PCM, caller-ID plan |
| [debrief](docs/debrief.md) | From-the-beginning briefing |

## Phone port quick start

Needs the OpenWrt **SDK for 25.12.5 ramips/mt7621** and a dump of your
router's **stock** firmware (`mtd4`), made before flashing OpenWrt. The
board's line settings are extracted from it.

```sh
export SDK=/path/to/openwrt-sdk-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64
cd fxs
./slicctl/fetch-vpapi.sh
python3 tools/extract_profiles.py /path/to/stock-mtd4.bin slicctl/stock_profiles.c
./build.sh                                     # -> fxs/build/*.apk (~10 min)
scp -O build/*.apk root@ROUTER:/tmp/
ssh root@ROUTER 'apk add --allow-untrusted /tmp/kmod-slic-spi-cs1-*.apk \
    /tmp/kmod-mt7621-pcm-raw-*.apk /tmp/slic-fxs-*.apk'
```

Then lift the handset for dial tone, `echo "ring 15" > /var/run/slic.ctl`, or
`slic-play /usr/share/slic/scale_c4_c5.s16`. As an OpenWrt feed:
`src-git zltphone https://github.com/udaravima/zlt-s12-pro-dev.git` (doc 09 §12).

**Safety:** the phone line carries up to ~100 V when active or ringing. Only
use line settings extracted from this board's own stock firmware (doc 09 §8).
The modem's boot chain is signed; these docs never reflash it.

## Licensing

- Documentation and `scripts/`: **MIT** (see `LICENSE`).
- Code in `fxs/` (kernel modules, `slicctl`, tools): **GPL-2.0** (see
  `fxs/LICENSE`). The kernel modules must be GPL.
- **Not included, on purpose:** Microchip/Microsemi VP-API-II (no
  redistribution licence; `fxs/slicctl/fetch-vpapi.sh` pulls it from the public
  GPL project that bundles it), Tozed line-setting bytes (regenerated from your
  own stock dump), and firmware dumps (device-unique data). So **don't
  redistribute built `slicctl` binaries or `slic-fxs` packages**; build them
  yourself.

## Credits

- MT7621 PCM register map: Puyou Lu's 2020 openwrt-devel ASoC patch.
- VP-API-II source as bundled by
  [pgid69/bcm63xx-phone](https://github.com/pgid69/bcm63xx-phone).
