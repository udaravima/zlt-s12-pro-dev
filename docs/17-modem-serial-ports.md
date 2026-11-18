# 17 — The modem's serial ports: what each one is, and who uses it

Checked live on 2026-11-18 from both ends: the router's USB view, and the
modem's own configuration over adb. Script:
[`scripts/router_modem_ports.sh`](../scripts/router_modem_ports.sh)
(`status` read-only, `apply` sets the assignment below).

## TL;DR

The LTE modem shows the router **five serial ports**. Two carry binary
streams for Unisoc's engineering tool; three are independent AT command
channels. Each AT channel now has one job:

| Router port | USB interface | On the modem | What it is | Used by |
|---|---|---|---|---|
| `ttyUSB0` | 08 | `ttyGS2` → atrouter channel 0 | AT commands | **people and tools** (`sms_tool`, manual AT) |
| `ttyUSB1` | 09 | `ttyGS0` → engpc | modem diagnostics, binary | nobody |
| `ttyUSB2` | 0a | `ttyGS1` → engpc | modem log stream, binary | nobody |
| `ttyUSB3` | 0b | `ttyGS3` → atrouter channel 1 | AT commands | **the LTE dial** (`network.wwan.device`) |
| `ttyUSB4` | 0d | `ttyGS4` → atrouter channel 2 | AT commands | **smsd** (`/etc/smsd.conf`) |

Interface 0c is adb. Interfaces 00–07 are four NCM network links (`usb0` is
the LTE data link; doc 05 uses `usb2` for the router↔modem link).

The two binary ports (`ttyUSB1`/`ttyUSB2`) are the "garbage" you see if you
open them. The modem pushes binary diagnostics and logs there whether anyone
reads them or not.

**Two traps** (§4):
- An AT port that no program has open collects the modem's unsolicited
  messages. The next program to open it reads that backlog first.
- The registration-report setting (`AT+CREG`/`+CEREG`) is shared by all
  three channels, as `AT+CPMS` is (doc 13).

---

## 1. How the ports drifted, and what that broke

The ports were assigned four times in two months, each time for a local
reason:

| Date | LTE dial (wwan) | smsd | Why (doc) |
|---|---|---|---|
| 2026-09-26 | `ttyUSB0` | none | the stock board script's default (08) |
| 2026-10-02 | `ttyUSB3` | none | ModemManager kept running for LuCI; it holds `ttyUSB0`+`ttyUSB4` when enabled (08) |
| 2026-10-13 | `ttyUSB3` | `ttyUSB0` | ModemManager now `disabled`, holding nothing (13) |
| after the 2026-11-05 reset | `ttyUSB4` | `ttyUSB4` | **not recorded** |
| 2026-11-18 | `ttyUSB3` | `ttyUSB4` | this doc |

The Nov 5 state put both on one port. It worked only because of start order:
at boot the network dials wwan (init order S20) before smstools3 (S94) opens
the port. smsd also logged every network-registration report it couldn't
place as `GSM1: Unexpected input: +CREG: … +CEREG: …`. That was 20 % of the
persistent system log over 14–17 Nov, and 377 of its last 500 lines on
18 Nov.

## 2. The map, from both ends

**Router side.** In sysfs, each `ttyUSBn` sits under one USB interface of
the modem (`1782:4055`, "Unisoc Phone"). The `option` driver numbers the
ports in interface order:

```sh
for t in /sys/class/tty/ttyUSB*; do
  echo "${t##*/} if=$(cat $t/device/../bInterfaceNumber)"
done
# ttyUSB0 if=08, ttyUSB1 if=09, ttyUSB2 if=0a, ttyUSB3 if=0b, ttyUSB4 if=0d
```

Interface 0c has class `ff/42/01` (the adb interface class) and no kernel
driver. ModemManager's own probe found AT on exactly `ttyUSB0`, `ttyUSB3`
and `ttyUSB4`. It rejected `ttyUSB1`/`ttyUSB2` as "unhandled port type".

**Modem side** (`adb shell` from the router). The USB gadget is configfs
`g1`. Its config `b.1` links ten functions, `f1`…`f10`:

| Link | Function | USB interface(s) | Modem device | Opened by |
|---|---|---|---|---|
| f1–f4 | `ncm.gs0`–`ncm.gs3` | 00–07 | — | (network) |
| f5 | `gser.gs2` | 08 | `/dev/ttyGS2` | `atrouter` |
| f6 | `gser.gs0` | 09 | `/dev/ttyGS0` | `engpc` |
| f7 | `gser.gs1` | 0a | `/dev/ttyGS1` | `engpc` |
| f8 | `gser.gs3` | 0b | `/dev/ttyGS3` | `atrouter` |
| f9 | `ffs.adb` | 0c | FunctionFS | `adbd` |
| f10 | `gser.gs4` | 0d | `/dev/ttyGS4` | `atrouter` |

The modem's config files say what each one does:

- `/etc/atrouter/system.ini`: `APChnl0=/dev/ttyGS2`, `APChnl1=/dev/ttyGS3`,
  `APChnl2=/dev/ttyGS4`, paired with `CPChnl0..2=/dev/stty_lte4..6`. So the
  three AT ports are **three separate channels** into the modem's cellular
  processor, each with its own command state.
- `/etc/engpc/dev/pc.conf`, `normal_lite` mode (the one running, judging by
  which devices `engpc` holds): `ttyGS0` = `COM_CP_DIAG`, `ttyGS1` =
  `COM_CP_LOG`. These are Unisoc's diagnostics and log channels, the ones
  their PC tools read.

**How sure the matching is.** The link names `f1`…`f10` give the order in
which the functions were added, and USB interfaces follow that order. Three
independent checks agree with it:
- adb lands on interface 0c, between `f8` and `f10`;
- ModemManager finds AT on exactly the three ports `atrouter` owns;
- the two binary ports are exactly the two `engpc` owns.

## 3. Who uses which port, and why

- **The LTE dial on `ttyUSB3`.** The dial only uses its port for a few
  seconds per connect (`gcom` chat scripts; doc 08 §2–3). ModemManager
  leaves `ttyUSB3` alone even when it enables the modem; it takes `ttyUSB0`
  (primary) and `ttyUSB4` (secondary). Doc 08 recorded a full dial cycle
  here with ModemManager running.
- **smsd on `ttyUSB4`.** smsd keeps its port open all the time
  (`keep_open = yes`), so it needs a port nobody dials on. After a factory
  reset the stock board script puts wwan on `ttyUSB0`, so keeping smsd off
  `ttyUSB0` means a reset without a backup can't recreate the clash.
- **`ttyUSB0` for people and tools.** Read-only queries are safe while smsd
  and wwan run.
- **ModemManager stays installed and `disabled`.** The 2026-10-02 decision
  kept it for LuCI. If anything enables it (`mmcli -m 0 -e`, a LuCI page),
  it would take `ttyUSB4` from smsd. `router_modem_ports.sh status` warns
  if its state isn't `disabled`.

What `apply` changed on 2026-11-18. Backups are in
`/root/backups/modem-ports-20261118-220951-1342/` on the router.

| Setting | Before | After |
|---|---|---|
| `network.wwan.device` | `/dev/ttyUSB4` | `/dev/ttyUSB3` |
| `network.wwan.delay` | (unset) | `5`: seconds `ncm.sh` keeps retrying the "who made you" query (`AT+CGMI`) before giving up |
| smsd `device` | `/dev/ttyUSB4` | `/dev/ttyUSB4` (unchanged) |
| smsd `init` | `AT` | `AT+CREG=0;+CEREG=0` |

## 4. The trap: messages pile up on a port nobody reads

**What happened during the move.** The first dial on `ttyUSB3` failed:

```
wwan: WARNING: Variable 'g:' does not exist or is not an array/object
wwan: Unsupported modem
```

`ncm.sh` asks the modem who made it (`AT+CGMI`). It treats the first
non-empty line of the reply as the answer and uses it as the key in
`/etc/gcom/ncm.json`. The answer should be `Spreadtrum Communication CO.`,
which gives the `spreadtrum` command set. Instead the first line was `g:`.
That's the tail of a `+CEREG:` registration report.

**Why a report was there.** Registration-report mode was 2 on every
channel (`AT+CREG?` → `+CREG: 2,…`). In mode 2 the modem sends a `+CREG:`
and `+CEREG:` line on every channel each time it changes cell, and this
modem changes cell often. Nobody had `ttyUSB3` open, so those lines piled
up for days. They arrived in a burst when the dial opened the port. The
dial's query script does drain the port first (`waitquiet 0.2 0.2` in
`getcardinfo.gcom`). The drain ended in a gap in the burst, and the rest of
a line arrived as the "answer".

*Assumed, not proven:* that the backlog waits on the modem side until the
port is opened. The evidence:
- the cut-off `g:`;
- the dial helper crashed the first time it opened each idle port (once
  with a bus error, once with exit 139), then worked 6 of 6 times.

**Why it never showed before.** smsd shared the dial's port and read
everything that arrived, reports included. That's where the "Unexpected
input" lines came from. The clash was hiding this trap.

**After the failure the link stays down.** "Unsupported modem" makes
`ncm.sh` mark wwan *unavailable*. `ifup` alone doesn't bring it back. The
only thing that marks it available again is the tty hotplug hook
(`/etc/hotplug.d/tty/30-3g`) when the modem's port appears again. By hand:

```sh
. /lib/netifd/netifd-proto.sh; proto_set_available wwan 1; ifup wwan
```

**The fix: turn the reports off.** smsd's init string is now
`AT+CREG=0;+CEREG=0`. It's sent on smsd's own channel, but the setting is
modem-wide: right after smsd restarted, `ttyUSB0` and `ttyUSB3` both
answered `+CREG: 0,…`. smsd stopped logging `+CREG` lines from then on.

- One line per dial still arrives on every channel: `^NDISDUN:usb0,enabled,…`
  (the modem announcing the data connection). That's a single line, and the
  0.2 s drain handles it. After the fix, a full re-dial worked; see §7.
- **After a modem restart the reports come back on until smsd's init runs
  again.** At router boot the dial (S20) runs before smsd (S94). But the
  modem has only just started, so there's no backlog, and that boot dial
  worked on 2026-11-14. *Not tested:* the modem restarting on its own while
  the router stays up.

## 5. Using a port by hand

Use `ttyUSB0`. `sms_tool` (package `sms-tool`; it isn't in
`scripts/router-packages.txt` yet) does one command and prints the reply:

```sh
apk add sms-tool
sms_tool -d /dev/ttyUSB0 at 'AT+CREG?'      # +CREG: 0,1,… = reports off, registered
sms_tool -d /dev/ttyUSB0 at 'AT+CPMS?'      # what smsd sees: CPMS is shared (doc 13)
```

- **Don't** send `AT+CPMS=…` or `AT+CREG=2`. Both are modem-wide, and they
  change what smsd sees.
- This router has no `stty`. A hand-made `cat`/`printf` probe therefore
  runs with the port's default settings, and those echo received
  characters back to the modem. Use `sms_tool` or `gcom` instead.
- Leave `ttyUSB1`/`ttyUSB2` closed. Reading them shows only binary.

## 6. Why plain `ttyUSBn` names, not stable aliases

A hotplug script could create aliases such as `/dev/modem-dial` from the
interface numbers. That breaks the LTE dial.
- After a failure, wwan is marked available again only by
  `/etc/hotplug.d/tty/30-3g`.
- That hook compares the *name* of the configured device (`${dev##*/}`)
  with the tty that just appeared.
- With an alias configured, it never matches, so after one failed dial the
  link would stay down until someone fixed it by hand.

The numbers themselves are stable: the `option` driver hands them out in
interface order. They would only shift if another USB serial device were
enumerated first. `router_modem_ports.sh status` checks each port's
interface number and warns if they have moved.

## 7. What was tested (2026-11-18)

| What | Result |
|---|---|
| `status` before the change | flagged `ttyUSB4` as configured for both `network.wwan` and smsd; exit 1 |
| `apply` | backups made; wwan moved; smsd restarted (IMEI/IMSI read, no "did not accept"); **the first dial failed** (§4) |
| Recovery | `proto_set_available` + `ifup`: up in 12 s, same lease |
| **Re-dial while smsd runs** (the case never tested before) | up in 12 s; smsd kept running |
| LTE data after the re-dial | 3/3 pings to 1.1.1.1 through a temporary route via `usb0`, 138–678 ms |
| `status` after | `OK: no clashes`, exit 0 |
| Registration reports | `AT+CREG?` = mode 0 on `ttyUSB0` and `ttyUSB3` after smsd's init; no `+CREG` lines from the new smsd process |

| SMS receive after the move | the owner texted the SIM: smsd logged `SMS received` at 23:18:44, and `incoming/` went from 9 to 10 files |

**Not tested:**
- a router reboot;
- the modem restarting on its own.

## 8. Found on the way: LTE isn't a backup right now

`network.wwan` has `defaultroute '0'`, and mwan3 isn't running. So the LTE
link comes up and gets an address, but no traffic uses it. If the fibre
(PPPoE) goes down, the house goes offline; nothing fails over to LTE. The
README's 2026-10-02 decision describes a metric-based backup (metric 50).
That isn't the current config: metric 20, no default route. This may be
deliberate, for example to keep data use off the SIM. It hasn't been
changed; it needs the owner's decision.

## How obtained

On 2026-11-18, all read-only except §3's change and the re-dials:
- router sysfs: `/sys/bus/usb/devices/*` (interface number, class, driver,
  tty);
- `mmcli -m 0`;
- `/proc/*/fd`, to see who holds which `ttyUSB`;
- on the modem, over `adb shell`:
  - `ls -l /sys/kernel/config/usb_gadget/g1/configs/b.1/`;
  - `cat …/functions/gser.gs*/port_num`;
  - `/proc/*/fd` for `ttyGS*`;
  - `grep ttyGS /etc/atrouter/system.ini /etc/engpc/dev/pc.conf`;
- `AT+CREG?` / `AT+CEREG?` on `ttyUSB0` and `ttyUSB3`;
- `gcom -s /etc/gcom/getcardinfo.gcom` on both ports, repeated;
- `logread` for netifd and smsd during `apply` and both re-dials.
