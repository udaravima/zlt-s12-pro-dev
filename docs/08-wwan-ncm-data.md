# 08 — WWAN data link: how the Mobitel link actually dials

*Written 2026-09-26, same day, after the link was brought up and after the
routing incident described in §4. Everything below was observed live on the
devices unless marked "theory".*

> **Update 2026-10-02 (hybrid NCM + failover state):** the architecture in
> this doc still holds, but the config and several §4/§8 facts changed:
>
> - **wwan moved to `/dev/ttyUSB3`** (was ttyUSB0) as a deliberate **hybrid**:
>   ModemManager now runs permanently (LuCI cellular page / SIM state) and
>   holds ttyUSB0+4; the ncm proto dials on ttyUSB3 with MM running — no port
>   collision observed across a full dial cycle. Current uci: proto ncm,
>   device /dev/ttyUSB3, apn mobitel, pdptype IP, metric 50, delay 5,
>   ifname usb0 (plus an inert `multipath='backup'` leftover). Verified
>   end-to-end: dial fires, usb0 gets **100.64.2.104/28 gw 100.64.2.97**
>   (CGNAT range changed since Sep 26's 100.64.3.81/30), default route
>   metric 50, ping via usb0 174–600 ms (EDGE). MM coexists throughout.
> - **slt_internet_wan got `metric='10'`** so the pppoe default (metric 10)
>   explicitly beats usb0 (metric 50) — no more metric-0 assumptions.
> - **mwan3 is NOT installed** (no /etc/init.d/mwan3, no opkg on this apk
>   build). `/etc/config/mwan3` is an inert leftover; its `wwan` section was
>   deleted. Failover is **pure metric-based routing**: pppoe default gone →
>   usb0 default (metric 50) becomes active. Demonstrated live once.
> - **Regression found (unresolved):** after a pppoe restart, the usb0
>   default route **vanished** — wwan_4 (dynamic dhcp iface) kept its IP but
>   its netifd route array went empty. Restored by `ifdown wwan; ifup wwan`.
>   Root cause unknown; **re-verify both default routes after any pppoe
>   restart** until understood. Also unexplained: during the one failover
>   demonstration pings via usb0 took 8–10 s (vs 200–600 ms normally).
> - **Keepalive installed (fragile):** `/etc/hotplug.d/iface/99-wwan-ping`
>   (background loop on wwan ifup) + crontab entry pinging the gateway via
>   usb0 every minute. Both **hardcode the DHCP-assigned gateway**
>   100.64.2.97 — they break silently if the lease changes.
> - **CGDCONT state:** CID 1 = `1,"IP","mobitel"` (set by ncm.sh, profile 1 —
>   good for a future LTE attach); CID 2 carries a corrupted APN
>   `"mobitela?"` (predates Oct 2); writes to CID 2 are refused
>   (`+CME ERROR 4`) — left alone, it's unused.

## TL;DR

| Question | Answer |
|---|---|
| Why did `wwan` sit "up" with no IP for days? | Bug in the netifd `ncm` proto handler + a stale uci option; the dial command was silently never sent (§3) |
| How does the dial actually work? | Router AT port → modem `atrouter` → **shell command exec on the modem** (`AT+SPTZCMD=<base64>`) → modem connmand → CP raises the PDP and binds it to NCM channel `usb0` (§1, §2) |
| Internet from modem? | Yes — after the fix, `usb0` gets 100.64.3.81/30 (CGNAT) from the modem and passes traffic |
| Why did the house internet break when it came up? | The dynamic DHCP interface installed a **metric-0 default route** that displaced the pppoe default; fix = `wwan.metric=50` so pppoe always wins (§4) |
| Who owns the default route? | `pppoe-slt_internet_wan` (SLT fibre), by design. The modem is secondary/failover-grade |

---

## 1. Architecture — five layers, two CPUs, one USB cable

```
┌─ ROUTER (MT7621, OpenWrt 25.12.5) ─────────────────────────────────┐
│ netifd interface "wwan"  (proto ncm, device /dev/ttyUSB0)          │
│   └─ /lib/netifd/proto/ncm.sh   shell proto handler                │
│        ├─ gcom chat on /dev/ttyUSB0 (/etc/gcom/runcommand.gcom)    │
│        ├─ vendor profile from /etc/gcom/ncm.json  → "spreadtrum"   │
│        └─ on success: spawns dynamic iface "wwan_4" (proto dhcp)   │
│             on usb0 + "wwan_6" (proto dhcpv6)                      │
│ kernel: huawei_cdc_ncm + cdc_ncm  → netdev usb0 (the data channel) │
└──────────────┬──────────────────────────────────────────────────────┘
               │ AT commands over /dev/ttyUSB0
               │ (USB serial ← modem ttyGS)
┌─ MODEM (SL8563, armv7) ─────────▼──────────────────────────────────┐
│ atrouter (the AT router daemon)                                    │
│   └─ AT+SPTZCMD="<base64>" = EXECUTE SHELL COMMAND AS ROOT         │
│        decoded:  connmanctl ndisdial 'AT^NDISDUP="usb0",1,1'        │
│   └─ modem connmand → oFono/RIL → CP (cellular processor)          │
│        └─ CP activates PDP context 1 (APN "mobitel") and binds     │
│           the bearer to the NCM instance named "usb0"              │
│ modem runs a DHCP server on that NCM instance                      │
└──────────────┬──────────────────────────────────────────────────────┘
               │ NCM data (same USB cable, different interface/altsetting)
               ▼
  router usb0: 100.64.3.81/30, gw 100.64.3.82 → Mobitel CGNAT
```

Key architectural facts, each of which cost debugging time:

1. **The router does not dial the modem; it asks the modem to dial itself.**
   The `ncm` proto's "connect" step is an AT command whose payload is
   base64 of a *shell command to run on the modem*: `connmanctl ndisdial
   'AT^NDISDUP="usb0",1,1'`. The modem's own connmand (via the RIL) asks
   the CP to raise the PDP context and bind it to the NCM channel the
   router will use as `usb0`. Disconnect is the same trick with `,0,1`.

2. **`AT+SPTZCMD` is a root shell-exec channel.** Any command base64'd
   into it runs on the modem as root (that is how `connmanctl` gets
   invoked). This is security-relevant: whoever owns the AT port owns the
   modem AP. We only ever saw it used for dial/disconnect, but the
   primitive is general. (Spreadtrum "TZ" = the command class name, not
   TrustZone — the exec is ordinary root on the AP.)

3. **Naming collision, deliberate but confusing:** there are *three*
   different things called "usb0":
   - the router's NCM **netdev** `usb0` (cdc_ncm/huawei_cdc_ncm),
   - the **modem-side NCM instance** the string `"usb0"` in the NDISDUP
     payload refers to,
   - one of the modem's own four NCM interfaces (`usb0..usb3` on the
     modem — `usb2` is the management link from doc 05).
   The router's data netdev and the modem instance are the same pipe; the
   modem's own `usb0` interface is unrelated to our mgmt `usb2`.

4. **After the dial, addressing is modem-driven**: the modem hands out
   100.64.3.81/30 (gateway .82) via DHCP to the router's dynamic
   `wwan_4` interface, and NATs/CGNATs from there. Verified working.

## 2. Component detail (developer level)

### 2.1 `proto_ncm_setup()` — `/lib/netifd/proto/ncm.sh`

Execution order, with what actually ran on this box (from logread):

| Step | Code | What happened here |
|---|---|---|
| 1 | resolve `device=/dev/ttyUSB0`, find `ifname` by walking `/sys/class/tty/ttyUSB0/../../<if>/net` | → `usb0` |
| 2 | `gcom getcardinfo.gcom` → manufacturer string, lowercased first word | → `spreadtrum` |
| 3 | `json_load /etc/gcom/ncm.json; json_select spreadtrum` | ok |
| 4 | replay `initialize[]` via gcom | `AT+CFUN=1`, `AT+CCED=2,8`, `AT+SPTTYROUTER=1` (all in log) |
| 5 | pincode (unset → skipped) | — |
| 6 | replay `configure[]` | `AT+CGDCONT=1,"IP","mobitel"` (in log) |
| 7 | **if `mode` uci option set**: `json_select modes` → look up `setmode` | **the bug trigger — see §3** |
| 8 | `json_get_vars connect` → replay via gcom | *silently skipped while bug active*; after fix: `AT+SPTZCMD="Y29ubmF…"=1,1` (dial) |
| 9 | `proto_init_update usb0` + `ubus network add_dynamic` | creates `wwan_4` (dhcp) and, because `ipv6='auto'`, `wwan_6` (dhcpv6) |
| 10 | `finalize` (unset for spreadtrum) | — |

### 2.2 `/etc/gcom/ncm.json` — the vendor profile DB (spreadtrum entry, verbatim)

```json
"spreadtrum": {
    "initialize": [ "AT+CFUN=1", "AT+CCED=2,8", "AT+SPTTYROUTER=1" ],
    "configure":  [ "AT+CGDCONT=${profile},\"${pdptype}\"${apn:+,\"$apn\"}" ],
    "connect":    "AT+SPTZCMD=\"Y29ubm1hbmN0bCBuZGlzZGlhbCBBVF5ORElTRFVOPSJ1c2IwIiwxLDE=\"",
    "disconnect": "AT+SPTZCMD=\"Y29ubm1hbmN0bCBuZGlzZGlhbCBBVF5ORElTRFVOPSJ1c2IwIiwwLDE=\""
}
```

Note: **no `modes` map** (unlike huawei/quectel/sierra entries), and no
`finalize`. The decoded payloads:

- connect:    `connmanctl ndisdial AT^NDISDUN="usb0",1,1`
- disconnect: `connmanctl ndisdial AT^NDISDUN="usb0",0,1`

### 2.3 gcom chat — `/etc/gcom/runcommand.gcom`

Dumb but reliable: sends `COMMAND` from the environment + CR, waits up to
25 s for `OK`/`ERR`/`ERROR`/`COMMAND NOT SUPPORT`, exit code reflects the
result. All netifd-logged `sending -> …` lines come from here.

### 2.4 Teardown

`proto_ncm_teardown()` reloads `ncm.json` **fresh** (does not reuse the
setup-time cursor), reads `disconnect`, dials it down. This asymmetry is
why disconnects always worked while connects silently didn't (§3).

## 3. The bug: a JSON cursor, a missing map, and a silent no-dial

### Mechanism

In `setup()`, step 7 of §2.1:

```sh
[ -n "$mode" ] && {
    json_select modes          # ← fails: spreadtrum has no "modes" map
    json_get_var setmode "$mode"
    ...
    json_select ..             # ← pops from CURRENT position = ROOT
}
...
json_get_vars connect          # ← now reads at ROOT level: empty
[ -n "$connect" ] && { ... }   # ← silently false: dial never sent
```

The uci config carried a stock leftover `option mode 'auto'`. Because the
spreadtrum profile has no `modes` object, the first `json_select` fails
and leaves the cursor unchanged (still inside the spreadtrum object —
this part is handled); the error is *visible in the log* as
`WARNING: Variable 'modes' does not exist or is not an array/object`.
The bug is the **unconditional** `json_select ..` afterwards: it assumes
the select succeeded and popped one level, so it actually pops from
spreadtrum → **root**. Every subsequent `json_get_vars` (`connect`,
`finalize`) then reads at root level, finds nothing, and the proto
declares success without ever dialing: interface "up", `usb0` NO-CARRIER,
no `wwan_4`, no IP. Netifd's own status even said `"up": true`.

Why the log made this findable: the successful teardown at 12:36 *did*
send an SPTZCMD, the failed setup right after it *didn't* — comparing the
two paths pointed straight at step 8.

### Fix (persisted)

```sh
uci delete network.wwan.mode    # RAT control lives on the modem anyway
uci commit network
```

Result (log, 12:41): `Configuring modem` → `Starting network wwan` →
**`Connecting modem`** → `AT+SPTZCMD="…,1,1` → `Setting up usb0` →
`wwan_4` DHCP → 100.64.3.81/30. The `modes` warning is gone too.

RAT mode needs no router-side option at all: network mode is persisted on
the modem itself (`tz.profile` `AT+SPTESTMODEM=6,254` = auto, doc 07).

## 4. Routing: the metric-0 collision (the outage)

### What happened, minute by minute

1. 12:15 — boot-time `ifup wwan` with old config (`apn hutch3g`, mode
   auto): no dial (§3), no routes.
2. ~12:36 — APN changed to `mobitel` and interface bounced (via LuCI).
   Still no dial (§3): log shows `CGDCONT=1,"IP","mobitel"` then straight
   to `Setting up usb0` with no `Connecting modem`.
3. 12:40–12:41 — `uci delete network.wwan.mode` + `ifup wwan` (the fix).
   Dial fired, `usb0` came up, `wwan_4` got 100.64.3.81/30 — **and its
   DHCP client installed `default via 100.64.3.82 dev usb0` with
   metric 0**, i.e. same metric as the pppoe default.
4. The pppoe default route was no longer in the table (only the usb0
   default remained). Side effects: the host-side session (SSH arriving
   over the SLT pppoe path) died mid-command, and the house lost
   internet. The operator restored service by hand:
   `route add default gw 100.64.4.34 dev pppoe-slt_inter`.

### Why it happened (verified + theory)

- **Verified:** both interfaces had metric 0, so the modem's DHCP default
  competed as an equal. The exact reason the pppoe default *disappeared*
  rather than coexisting (route replace vs. add somewhere in the
  netifd/udhcpc path) is **unverified** — marked as theory. It doesn't
  matter for the fix: metrics must differ.
- **Design rule now enforced:** the modem is a *secondary* uplink.
  `network.wwan.metric=50` (pppoe stays metric 0). The metric propagates
  to the dynamic `wwan_4` because `proto_add_dynamic_defaults` copies the
  parent proto's default options into the dynamic interface it spawns —
  so the DHCP route lands with metric 50 and never wins over pppoe.

### Current intended routing state

```
default via 100.64.4.34  dev pppoe-slt_inter   (metric 0, primary)
default via 100.64.3.82 dev usb0  metric 50  (secondary, failover-grade)
```

### Related config facts (observed, not changed this session)

- Firewall: `wwan` (and `wwan2`) are in the `wan` zone (masq on) —
  LAN→modem NAT works the moment routing sends traffic there. The zone's
  `network` list contains ~10 duplicate `'wwan'` entries (harmless LuCI
  artifact, worth cleaning during router hygiene).
- mwan3 is installed but not usable as-is: its `wan` section tracks a
  netifd interface literally named `wan` which doesn't exist in this
  config (the pppoe iface is `slt_internet_wan`), and `wanb` is
  disabled. Proper mwan3 failover = rename sections to real interface
  names + enable. Deliberately deferred (see open items).

## 5. Session narrative (what / why / expected / found)

- **Asked:** "no internet from modem — get the link up; also document at
  developer-architecture level."
- **Expected at the start:** the `ncm` proto would dial like any OpenWrt
  NCM modem; failure would be modem-side (no PDP, bad APN).
- **Found instead:** the proto never even attempted the dial (§3); the
  modem-side machinery (SPTZCMD → connmand → CP → NCM bearer + DHCP) works
  flawlessly once actually invoked.
- **Expected after the fix:** green interface, internet from modem — and
  got it, but at the cost of the pppoe default route (§4): a failure mode
  I did not anticipate because both metrics were 0 and I didn't check the
  route table before/if after `ifup`. Lesson recorded: **any new uplink
  gets a non-zero metric before first `ifup`**.
- **Operator intervention that restored service:** manual
  `route add default gw … dev pppoe-slt_inter` (runtime-only, self-heals
  on reboot/pppoe restart when netifd re-adds its own default).

## 6. Persisted changes on the devices (cumulative, all sessions)

### Router (`/etc/config/network`, committed via uci)

| Change | By | Session |
|---|---|---|
| `wwan.apn='mobitel'` (was stale `hutch3g`) | operator (LuCI) | this one |
| `wwan.mode` deleted (was `auto`; bug trigger, §3) | agent | this one |
| `wwan.metric='50'` (route priority, §4) | agent | this one |

Runtime-only state (not persisted, resets on reboot): ModemManager
stopped; user's manual default route; usb2 mgmt link down since the modem
reboot (unpersisted by design until next-step 3).

### Modem (`/mnt/userdata`, survives reboot)

| Change | By | Session |
|---|---|---|
| `tz.profile`: `AT+SPTESTMODEM=3,254` → `6,254` (LTE-only → auto) | agent | doc 07 |
| backups: `/mnt/userdata/tz.profile.bak-20260926`, host copy `tzprofile-backup-20260926.txt` | agent | doc 07 |
| `/mnt/userdata/tools/modem_send.py` (transfer tool) | agent | doc 05 |

### Untouched on principle

Modem rootfs (read-only anyway), `ril.ini` (self-maintained by the RIL),
`/proc/cptl/*`, firewall/mwan3 config, everything under `/etc/config/`
except `network` rows listed above.

## 7. Safe bring-up / verification procedure (for next time)

```sh
# 0. metric FIRST — never ifup a new uplink with metric 0
uci set network.wwan.metric='50'; uci commit network
# 1. bring up
ifup wwan
# 2. watch the dial (must show "Connecting modem" + SPTZCMD ,1,1)
logread -f | grep 'wwan ('
# 3. verify routing priority and data path
ip route show default          # pppoe line must be first / lower metric
ping -c3 -I usb0 8.8.8.8       # modem path works independently
ping -c3 8.8.8.8               # default path (pppoe) still primary
```

## 8. Open items

1. **mwan3 failover** — sections need renaming to real interfaces
   (`slt_internet_wan` / `wwan`), `wanb` enabling, policy rules; only
   then does the modem link take over automatically when SLT dies.
2. **LTE absence** — link currently rides GSM/EDGE (CGNAT, high latency).
   If Mobitel LTE is still invisible in known-LTE coverage, suspect
   `AT+SPLBAND=1,0,128,0,0,0` in `tz.profile` (doc 07).
3. **`wwan_6` / ipv6='auto'** — dynamic dhcpv6 iface spawns each ifup;
   untested whether the modem serves v6 at all.
4. **Firewall zone cleanup** — duplicate `wwan` entries in wan zone
   network list (cosmetic).
5. **AT+CGATT residual context** — modem had a network-activated PDP with
   blank APN before we ever dialed (doc 07); harmless but worth rechecking
   with the real dial in place.

## How obtained (all live, 2026-09-26)

- `ifstatus wwan`, `logread | grep 'wwan ('` (the authoritative trace)
- `cat /lib/netifd/proto/ncm.sh`, `cat /etc/gcom/ncm.json`,
  `cat /etc/gcom/runcommand.gcom`
- base64 payloads decoded off-host: `echo <b64> | base64 -d`
- `uci show network|firewall|mwan3`, `route -n`, `ip route`, `ip addr`,
  `ping -I usb0`
