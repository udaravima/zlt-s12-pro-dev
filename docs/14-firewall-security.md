# 14 — Firewall security check-up and provisioning scripts

Checked live on 2026-11-15 (OpenWrt 25.12.6 r33311, firewall4). Scripts:
[`scripts/router_fw_audit.sh`](../scripts/router_fw_audit.sh) (read-only
check-up) and
[`scripts/router_fw_provision.sh`](../scripts/router_fw_provision.sh)
(puts the router into the state the audit checks for).

**Status:** audit run live; provisioning dry-run live and `--apply` tested on
copies of the config files. **Not yet applied to the live router.**

---

## 1. The verdict

**Nothing is exposed.** The audit's result is 0 FAIL and 8 WARN. Both firewall
policies that matter are `REJECT`: input from the WAN and forwarding from the
WAN. There are no port forwards, UPnP is off, and Asterisk's control
interfaces (AMI, ARI) are either off or bound to `127.0.0.1`. In the 11 h
53 min before the check, the firewall turned away **45,216 packets from the
WAN, about one per second**. That is normal background scanning, and it shows
the wall is in use, not theoretical.

The WARNs share one pattern: **for several services, the firewall is the only
layer of protection.** SSH, nginx (ports 80 and 443, including the reverse
proxy to Asterisk's ARI) and a few small daemons listen on every address. A
single firewall mistake, such as a port forward added in LuCI or an uplink
placed in the wrong zone, would expose them directly. The provisioning script
adds the missing second layer.

The interesting find is a stock OpenWrt bug, not a security hole: the
board script for this exact router model adds a duplicate `wwan` to the WAN
zone on every boot (§3). That is where the 15 duplicate entries noted back in
doc 08 came from.

## 2. Who can reach the router at all

| From | Path | What the firewall allows |
|---|---|---|
| The open internet | Can't start a connection. The PPPoE address is in carrier-grade NAT space (100.64.0.0/10), and the LTE address is a carrier-private `10.x`. | n/a |
| The carrier's own network (other subscribers behind the same CGNAT, ISP equipment) | `pppoe-slt_inter` and `usb0`, both in zone `wan` | DHCP replies, ping, IGMP, ICMPv6. Everything else is rejected. |
| The modem | `usb0` | same as above (it's a WAN device) |
| Anything on the LAN or Wi-Fi | zone `lan`, input `ACCEPT` | everything: SSH, LuCI, SIP, DNS |

The second row is the realistic outside threat. CGNAT hides you from the
internet, not from your neighbours on the carrier's network. The 45,000
rejected packets came from that side.

The fourth row matters more than it looks. Every Wi-Fi client is a full LAN
member, because there is no guest network. So any device on the Wi-Fi can try
to guess root's SSH password, and nothing slows it down (§5, SSH).

## 3. The duplicate `wwan` that comes back after every boot

The chain:

1. Early in every boot, `/lib/preinit/10_indicate_preinit` runs
   `board_detect /tmp/board.json`, only to decide how the LEDs should
   behave.
2. `board_detect` runs every script in `/etc/board.d/`. The
   `tozed,zlt-s12-pro` branch of `/etc/board.d/02_network` (stock OpenWrt,
   identical to `/rom`) contains
   `uci add_list firewall.@zone[1].network='wwan'`.
3. That line has no "is it already there?" check, and nothing commits it.
   The change sits **staged** in `/tmp/.uci/firewall`. Its timestamp is
   1970-01-01 05:30:09, because the clock hasn't been set yet that early.
4. LuCI shows it as **1 unsaved change**. The next **Save & Apply**, on any
   LuCI page, commits it together with your own change.
5. Result: one more `list network 'wwan'` per boot-plus-save. Verified on a
   copy: current uci appends a duplicate for each staged add. Two adds were
   staged (one by me, plus the live boot line, see §4.3), and the copy went
   from 15 to 17 `wwan` entries.

**What breaks:** nothing visible. fw4 collapses duplicates when it builds the
ruleset. The cost is confusion: a config that grows by itself, a permanent
"unsaved change", and a trap for any script that commits `firewall` (it
silently commits this too).

**Don't just delete the line from `02_network`.** On a freshly reset router,
that staged line is the only thing that puts `wwan` into the WAN zone. Both
fixes in the provisioning script therefore act only once `wwan` is already
committed:

- **Now:** `uci revert firewall`, but only when the staged changes consist
  solely of that line.
- **At every later boot:** an equally guarded block in `/etc/rc.local`,
  inserted before its `exit 0`. `rc.local` is in the sysupgrade backup.

This is worth reporting upstream: OpenWrt's ramips `02_network`, branch
`tozed,zlt-s12-pro`. Not filed yet.

## 4. The scripts

Both run on the router under BusyBox ash and are piped over SSH, the same
way as the other router scripts. Use `/usr/bin/ssh`, not the local wrapper.

```sh
# the check-up: read-only, exit status 1 if anything FAILs
/usr/bin/ssh root@<router> 'sh -s' < scripts/router_fw_audit.sh

# provisioning: dry run first (prints the plan, writes nothing)
/usr/bin/ssh root@<router> 'sh -s' < scripts/router_fw_provision.sh
# then for real
/usr/bin/ssh root@<router> 'sh -s -- --apply' < scripts/router_fw_provision.sh
# then re-run the audit; a second --apply should print "nothing to do"
```

### 4.1 What `--apply` changes, and why

The plan from the live dry run on 2026-11-15 had 9 changes, grouped by what
each protects:

| Group | Change | Why |
|---|---|---|
| Firewall config hygiene | drop the staged boot-time `wwan`; rewrite zone wan's network list from 21 entries to 6 (duplicates out, order kept); add the rc.local guard | §3 |
| WAN → LAN | remove the stock rules `Allow-IPSec-ESP` and `Allow-ISAKMP` | They forward *unsolicited* ESP and UDP 500 from the WAN to LAN hosts. A VPN client behind NAT doesn't need them, because its replies are already `established`. They only matter for packets addressed straight to a LAN IP, which only the ISP's next hop or the modem could send. Low risk, but zero use. `--keep-ipsec` keeps them. |
| SSH | `PasswordAuth off`, `RootPasswordAuth off` | Stops password guessing from LAN and Wi-Fi devices. One ed25519 key is installed, and the script refuses this step when `authorized_keys` has no key. `--keep-ssh-passwords` skips it. |
| SSH | `Interface` = the LAN network (`lan_int`) | dropbear then binds to the LAN address only, so a firewall mistake can't expose it. `--ssh-any-interface` skips it. |
| nginx | add `include restrict_locally;` to `conf.d/polarsip.conf` | The LuCI site already answers only LAN and loopback sources. The ARI proxy site answered any source, and only the firewall kept the WAN out. ARI still needs its own password; this adds the second wall. |

It also **ensures**, and today changes nothing for:
- the policies (defaults and zone wan input/forward `REJECT`; `DROP` is kept
  if it's already set), plus SYN-flood limiting, masquerade and MSS clamping;
- the DNS policy: LAN DNS redirected to the router, and DNS-over-TLS (853)
  to the WAN blocked. `--no-dns-policy` skips this.
- UPnP stays disabled.

It **reports, never removes,** port forwards from the WAN. It never touches
zone lan, Asterisk's own config (`slic-asterisk-setup` owns that) or whether
an interface is up. `voip_br` stays where it is (disabled, listed in zone
wan, so it's firewalled if it's ever enabled).

`WAN_IFACES` (default `slt_internet_wan wwan wwan2 slt_voip_wan
slt_iptv_wan voip_br`) lists what belongs in zone wan after a reset. Names
that don't exist in `/etc/config/network` are skipped. A name that sits in
zone lan is reported and not moved, because an uplink in lan is the one
genuinely dangerous mistake, and moving it should be a deliberate act.

### 4.2 Safety rails

- **Dry run by default.** Nothing is written without `--apply`.
- **Unsaved-changes check.** If `firewall`, `dropbear` or `upnpd` has staged
  changes (other than the §3 line), the script stops. Otherwise it would
  commit someone's half-finished LuCI edit along with its own changes.
- **Backup first.** Before writing, it copies every file it may touch to
  `/root/backups/fw-provision-<time>/`.
- **Validate before reload.** It reloads the firewall only if `fw4 check`
  passes; otherwise it restores the backup. It reloads nginx only if
  `nginx -t` passes; otherwise it restores `conf.d`.
- **SSH last.** dropbear reloads at the very end. The running SSH session
  stays open. **Open a second session and log in before closing the first.**
  If locked out, LuCI still accepts the root password: System →
  Administration → SSH Access turns password login back on.
- uci rewrites `/etc/config/dropbear` on commit and drops its comment
  lines. That's normal uci behaviour, and the backup has the original.

### 4.3 Test mode, and its trap

```sh
# on the router: copy the configs, then point the script at the copies
T=/tmp/fwtest; mkdir -p $T/conf $T/nginx
cp -p /etc/config/firewall /etc/config/network /etc/config/dropbear /etc/config/upnpd $T/conf/
cp -pr /etc/nginx/conf.d /etc/nginx/restrict_locally $T/nginx/; cp -p /etc/rc.local $T/
UCI_CONFDIR=$T/conf NGINX_DIR=$T/nginx RC_LOCAL=$T/rc.local sh router_fw_provision.sh --apply
```

`UCI_CONFDIR` implies no reloads. Run on 2026-11-15, the live files stayed
byte-identical (md5), the copies got exactly the planned changes, and the
inserted rc.local block passed `sh -n`.

**Trap:** `uci -t <dir>` does not isolate a test. libuci always keeps its
default staging folder `/tmp/.uci` on the search list. So a test run *sees*
the live staged `wwan` line but can't revert it: on every repeat it plans
that drop and a 7→6 dedupe again. Live runs do converge. The real idempotence
check is a second live `--apply` printing "nothing to do".

## 5. Every check, what it means, and today's result

Verified live on 2026-11-15 by running the audit.

| Check | Result | What a bad result would mean |
|---|---|---|
| `fw4 check` | PASS | The ruleset doesn't compile, so the firewall may not be loaded at all. |
| Default and zone wan `input`/`forward` | PASS, all `REJECT` | `ACCEPT` here opens every router service, or the LAN, to the WAN. |
| Each live interface's zone | PASS: LAN bridge → lan; `pppoe-slt_inter`, `usb0` → wan | FAIL if an uplink (pppoe, dhcp, ncm, qmi…) lands in a zone with input `ACCEPT`. An unzoned interface gets the default `REJECT`: broken, not exposed. |
| Port forwards from the WAN | PASS, none | FAIL if one hits SSH/DNS/web, SIP 5060, RTP 10000–10199, ARI 8088 or AMI 5038. |
| Other WAN `ACCEPT` rules | WARN, the two IPsec rules (§4.1) | |
| IPv6 | INFO: no global address | With global IPv6, every LAN host is addressable from outside; `forward_wan` is then its only shield. |
| Listeners on all addresses | INFO: SSH 22, nginx 80/443, dnsmasq DHCP 67, usteerd 16720 | Firewall-only protection, so each needs a second layer where possible. |
| SSH | WARN ×2: all interfaces; passwords accepted | |
| Root password | PASS (sha256-crypt) | An empty password opens LuCI and SSH to the whole LAN. |
| nginx sites | PASS LuCI (`restrict_locally`); WARN the ARI proxy site | |
| Asterisk | PASS: SIP on the LAN address only, AMI off, HTTP/ARI on `127.0.0.1` | AMI on a real address hands out full PBX control. |
| UPnP / SNMP | PASS: UPnP installed but off; snmpd not enabled | UPnP lets any LAN device open WAN ports unasked. |
| Kept by the sysupgrade backup | PASS: firewall, dropbear, `authorized_keys`, `restrict_locally`, `polarsip.conf` | A missing entry means the hardening silently vanishes on the next reset. |

The backup `.tar.gz` contains the nginx TLS private keys and the SIP
passwords (`pjsip_secrets.conf`). **Store it like a password.**

## 6. Considered and deliberately left alone

- **`drop_invalid`, strict `rp_filter`.** Little gain behind a REJECT
  policy. Strict reverse-path filtering breaks asymmetric multi-WAN routing
  (pppoe + LTE, doc 08).
- **WAN ping (`Allow-Ping`).** A stock rule, useful for the ISP's
  diagnostics. It reveals only that something is there, which CGNAT already
  shows.
- **LAN zone input `ACCEPT`, no guest Wi-Fi.** Changing either is a network
  redesign, not hardening. Worth it if untrusted devices (IoT, visitors)
  join the Wi-Fi: a guest network in its own zone, with DNS and DHCP only
  to the router.
- **DNS-over-HTTPS** isn't blocked; only DoT (853) is. Blocking DoH needs
  IP lists, which is policy, not security.
- **Installed but idle packages** (snmpd, emailrelay, openvpn, clamav): not
  exposed today. Removing them is router hygiene (debrief item 4).

## 7. Open items

1. Apply the provisioning live (needs the owner's go-ahead: it switches SSH
   to key-only). Then re-run the audit, and run `--apply` a second time
   expecting "nothing to do".
2. Reboot once after applying, to check the rc.local guard. LuCI should
   show no unsaved change after boot, and the zone should still list 6
   networks.
3. Report the `02_network` bug upstream (§3).
4. A guest Wi-Fi zone, if untrusted devices ever join (§6).

## How obtained

- `uci show firewall|dropbear|uhttpd|nginx|upnpd|wireless|network`,
  `nft list chain inet fw4 input|input_wan|forward_wan|dstnat|reject_from_wan`,
  `fw4 check`, `netstat -tulnp`, `sysupgrade -l`, `/proc/uptime`.
- `/etc/nginx/uci.conf`, `conf.d/*`, `/etc/asterisk/{http,manager,ari,rtp}.conf`
  (keys and passwords masked in all output).
- The boot-time duplicate: `uci changes firewall`,
  `ls --full-time /tmp/.uci/firewall /tmp/board.json`, the
  `board_detect` callers (`grep -r board_detect /lib/preinit /bin`),
  `/etc/board.d/02_network` vs `/rom`, and a staged-commit test on a copy.
- Script tests: live audit; live dry run; `--apply` on copies with md5
  checks of the live files before and after.
