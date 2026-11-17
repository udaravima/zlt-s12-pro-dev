# 16 — Phone pages in LuCI: status and line settings

The router's web interface (LuCI) gains a **Services → Phone** menu with two
tabs:

- **Status** (read-only): the RJ11 phone line, and the Asterisk PBX if one is
  installed. It shows the SIP phones and where they registered from, active
  calls, HEP capture, and recent problems from Asterisk's log. It also raises
  a warning when a module can't hook into Asterisk's SIP stack.
- **Phone line**: a form over `/etc/config/slic`, the line daemon's settings.

The package is `luci-app-slic-fxs`, in [`fxs/luci-app-slic-fxs/`](../fxs/luci-app-slic-fxs).
It is architecture-independent (JavaScript, JSON and one `ucode` script, no
compiled code), and needs nothing the router doesn't already run: LuCI,
rpcd and `ucode`.

Written 2026-11-17. **The backend and the Save & Apply path were tested on the
router; the pages themselves have not yet been looked at in a browser** (§7).

Building this turned up a bug that predates it: **a settings change often
never reached the phone line daemon.** procd compares a service's
environment without the last 4 bytes of each entry, so it missed changes such
as dial timeout 3 → 4. `slic-fxs` r6 restarts the daemon explicitly (§4).

---

## 1. What the pages show

### 1.1 Status

Refreshes every 10 s while open (LuCI's refresh indicator pauses it).

| Section | Row / column | Meaning | Where it comes from |
|---|---|---|---|
| Phone line | Daemon | running (with PID), not running, or switched off | procd (`ubus call service list`) and `slic.main.enabled` |
| | Line | idle, dial tone, dialling, ringing, in a call, busy tone | `/var/run/slic.state` (doc 10 §3.2) |
| | Handset | lifted or on hook | same file, `hook=` |
| | Current or last call | ringing, answered, connecting, connected, ended, busy | `/var/run/slic.call` |
| | Caller ID format, Dial timeout | the values the daemon **is running with** | the daemon's own environment, `/proc/<pid>/environ` (not procd's record: §4) |
| | Kernel modules | loaded, or which is missing | `/proc/modules` |
| | Recent line events | the last 8 lines from the daemon and the init script | `logread` (tags `slicctl`, `slic`) |
| PBX | Asterisk | running (version, PID) or not | `/var/run/asterisk/asterisk.pid` + `/proc/<pid>/comm`; version from `asterisk -V` once per start |
| | Up for | time since Asterisk started | `/proc/<pid>/stat` field 22 against `/proc/uptime` |
| | HEP capture | on (and to where), enabled but not loaded, off | `/etc/asterisk/hep.conf` + whether `res_hep_pjsip.so` is mapped in `/proc/<pid>/maps` |
| SIP phones | Extension, State | each PJSIP endpoint and its device state (idle, in a call, not registered, …) | `asterisk -rx 'pjsip show endpoints'` |
| | Registered from | address, port, and transport if Asterisk printed it in full | the endpoint's `Contact:` lines |
| | Reachability | Asterisk's own check (`qualify`) and its round trip in ms | same lines. Phones that sleep (mobiles) answer slowly: 2 s is normal for them |
| Active calls | one row per channel | channel, state, caller ID, number@context, application, duration | `asterisk -rx 'core show channels concise'`. A call between two parties usually has two channels |
| Recent problems | the last 10 | errors, warnings, and refused SIP requests (`… failed for …`: a wrong password shows here). Known-harmless lines are **labelled, not hidden**: "the canary is no more", "no local system nameservers", and music on hold with no files (doc 12 §7) | Asterisk's `messages.log`, read incrementally |

### 1.2 Warnings at the top of the status page

| Warning | Cause | What to do |
|---|---|---|
| modules started loading and never finished | the boot-loop guard `/etc/slic.loading` is set: a module load crashed or hung at an earlier boot (doc 10 §4.1) | read the system log; then `rm /etc/slic.loading && /etc/init.d/slic start` |
| the phone line daemon is not running | enabled, but procd has no running instance | the "Recent line events" list and `logread -e slic` |
| saved settings differ from what the daemon runs with | `/etc/config/slic` was changed without a restart | `/etc/init.d/slic restart` (Save & Apply does it) |
| Asterisk is not running | — | the handset then gets busy tone on every number (doc 12 §7) |
| N module(s) could not hook into the SIP stack | since Asterisk started, `Unable to register module …` was logged: PJSIP's module table is full (doc 12 §7.1) | give up a slot with `noload` for an unused `res_pjsip_*` module |
| HEP enabled but not loaded | `hep.conf` says yes, `res_hep_pjsip.so` isn't mapped | `scripts/router_hep.sh status` |

### 1.3 Phone line

| Field | Option | Values | Takes effect |
|---|---|---|---|
| Enabled | `enabled` | on/off. Off stops the daemon at once; the kernel modules stay loaded until the next reboot, after which they aren't loaded at all | on Save & Apply |
| Caller ID format | `cid_format` | Bell 202 FSK after the first ring (`bell`), ITU-T V.23 FSK before it (`v23`), DTMF before it (`dtmf`). Bell is the one confirmed on the test phone | on Save & Apply |
| Dial timeout | `dial_timeout` | 1–10 s after the last key; `#` dials at once. Default 3 | on Save & Apply |
| AudioSocket address *(Advanced)* | `audiosocket` | `127.0.0.1:PORT` or `off`. **Nothing else is accepted**: the daemon listens there, and any other address would let the network reach the phone's audio. Asterisk's dialplan connects to the same address, so change both | on Save & Apply |
| Audio clock *(Advanced)* | `clk_switch` | router (`0`: needed for calls, caller ID and audio) or modem (`1`: the stock VoLTE wiring; ringing works, nothing else) — doc 09 §5.3 | **next reboot** (kernel module parameter) |
| SPI clock *(Advanced)* | `spi_hz` | 100 000–2 048 000 Hz; 1 000 000 is tested, the stock firmware used up to 2 048 000 | **next reboot** (kernel module parameter) |

Save & Apply restarts the daemon (about 2 s), which **cuts off a call in
progress**. An empty field means "use the default".

---

## 2. Install, update, remove

The pages need `slic-fxs` **r6 or later**: with r5, many settings changes are
saved but never reach the daemon (§4). The status page still tells the
truth in that case: it shows the "settings differ" notice.

```sh
# build (with the rest of fxs/, doc 09 §10): fxs/build/luci-app-slic-fxs-*.apk
cd fxs && SDK=/path/to/openwrt-sdk-25.12.x-ramips-mt7621_... ./build.sh
scp -O build/luci-app-slic-fxs-*.apk build/slic-fxs-*.apk root@ROUTER:/tmp/
ssh root@ROUTER 'apk add --allow-untrusted /tmp/slic-fxs-*.apk /tmp/luci-app-slic-fxs-*.apk'
```

Then reload the LuCI page: **Services → Phone** appears for any login allowed
the `luci-app-slic-fxs` access group (root always is). Installing clears
LuCI's menu cache and reloads rpcd, so no reboot is needed. Remove it with
`apk del luci-app-slic-fxs`.

As an OpenWrt feed (doc 09 §12) the package is `luci-app-slic-fxs` under
LuCI → Applications. Its Makefile uses plain `package.mk`, not `luci.mk`, so
it also builds in an SDK without the luci feed. That SDK doesn't know
`luci-base` and prints a warning for the dependency. Nothing is lost on the
router side, which has LuCI anyway.

---

## 3. How it works

```
browser ── LuCI (view/slic/status.js, line.js) ──► rpcd (session + ACL)
                                                    │
        status page: ubus luci.slic status ─────────┤──► /usr/share/rpcd/ucode/luci.slic
                                                    │      reads: state/call files, /proc, procd,
                                                    │      logread, Asterisk log, hep.conf,
                                                    │      asterisk -rx (2 commands)
        form: uci set/commit (Save & Apply) ────────┘──► /etc/config/slic
                                                           │ config.change event
                                                           ▼
                                                 procd ── /etc/init.d/slic reload
                                                           = stop + start (§4)
```

**Files of the package:**

| File | Role |
|---|---|
| `/www/luci-static/resources/view/slic/status.js` | the status page (renders in the browser) |
| `/www/luci-static/resources/view/slic/line.js` | the form (`form.Map` over `slic`) |
| `/usr/share/luci/menu.d/luci-app-slic-fxs.json` | menu entry; shown only with the ACL and when `/etc/config/slic` exists |
| `/usr/share/rpcd/acl.d/luci-app-slic-fxs.json` | access: `luci.slic status`, read/write of the `slic` config. Nothing else |
| `/usr/share/rpcd/ucode/luci.slic` | the backend: one method, `status`, no arguments |

**Cost to the router, measured on the MT7621 (880 MHz):**
- **Each refresh: 0.42 s.** Almost all of it is the two `asterisk -rx` calls
  at 0.15 s each.
- **The first refresh after rpcd starts: 1.9 s.** It reads all of Asterisk's
  372 KB log once; after that, only what's new.
- **`logread`: 0.43 s**, because it fetches the whole system log buffer. So it
  runs only when the line's state or call file changed, or once a minute.

At one refresh per 10 s that's about 4% CPU while a status page is open, and
nothing when none is. Things that cost a process each were replaced by file
reads:
- "is this module loaded" comes from `/proc/<pid>/maps`;
- uptime comes from `/proc/<pid>/stat`;
- the version is cached per Asterisk process.

**Security:**
- **Access:** the pages sit behind LuCI's login, served by nginx on the LAN
  only (doc 14). The access group allows one read-only method and the `slic`
  config, nothing else.
- **The backend accepts no input:** every command it runs is a constant
  string.
- **Secrets:** it never opens `pjsip_secrets.conf` or `ari.conf`, so no
  password can reach the browser. Log lines can contain phone numbers and LAN
  addresses, which is normal for an admin page.
- **No markup from data (stored XSS).** Much of what the page shows comes
  from outside:
  - caller IDs and contact URIs;
  - log lines that quote the `From` header of *unauthenticated* SIP
    requests.

  LuCI's `E(tag, attrs, child)` puts a *single string* child in as HTML
  (`innerHTML`); only strings inside an *array* become text. One bare string
  would let any device on the LAN send a SIP request whose `From` header
  then runs as script in an administrator's LuCI session, which is root.
  So `status.js` passes every child in an array. The first draft didn't,
  and an automated security review caught it before release; §7 has the
  test that proves the fix.

---

## 4. The bug found on the way: settings that never reached the daemon

**Symptom:** change the dial timeout from 3 to 4 and Save & Apply (or
`uci commit slic` + `/etc/init.d/slic reload`). procd's record of the
service (`ubus call service list`) says `SLIC_DIAL_TIMEOUT=4`. But the
daemon keeps its PID, and `/proc/<pid>/environ` still says 3, for good:
watched for 12 s, three times.

**The chain:**
1. The init script hands the settings to the daemon as environment variables
   and registers a reload trigger. On a config change, procd re-runs the init
   script, gets the new instance definition, and asks `instance_config_changed`
   whether to restart.
2. For the environment, that calls `blobmsg_list_equal`. It does
   `memcmp(n1->data, n2->data, blob_len(n1->data))`, but `data` points at the
   attribute's 4-byte **header**, while `blob_len` is the **payload** length.
   So it compares the header plus the payload minus its last 4 bytes.
3. In `SLIC_DIAL_TIMEOUT=3` the value sits entirely in those last 4 bytes. The
   change is invisible, procd decides nothing changed, updates its record,
   and leaves the process alone. The same goes for an AudioSocket port change
   (4573 → 4574).

**The theory was tested by its predictions.** A change in length (3 → 10, then
back 10 → 3) *was* detected and restarted the daemon both times; 3 → 4 never
was. Seen on procd 2026.03.13~58eb263d, with the code as in procd's master
branch on that day.

**Fix (slic-fxs r6):** `/etc/init.d/slic` defines `reload_service() { stop; start; }`,
so every config change restarts the daemon. `procd_lock` is re-entrant, so
`stop` inside `reload` can't deadlock. **Verified on the router:**
- 3 → 4 gave a new PID with 4 in its environment;
- 4573 → 4574 did the same;
- restoring both did it again, each restart taking about 2 s.

**Second consequence:** the status page reads the daemon's real environment
from `/proc`, not procd's record, which can show values the process never
received.

**Upstream:** worth reporting to procd. The fix is to compare `blob_data()` of
both attributes, or `blob_raw_len()` bytes from the header. Not filed yet.

---

## 5. Limitations

- **Asterisk's CLI text is parsed** (`pjsip show endpoints`). It's written for
  people and may change between versions; checked against Asterisk 23.1.
  - **Contact URIs are cut to the column width**, so a long one can lose its
    `;transport=…` (that already happens on this router for a TCP phone), or
    even its port.
- **Log times have no year:** "since Asterisk started" can misjudge lines
  around New Year. It also assumes logger.conf's default date format; lines
  in other formats count as current.
- **No push:** the page polls every 10 s. A ringing call appears on the next
  refresh.
- **English only:** the strings go through LuCI's `_()`, but no translations
  are shipped.
- **Only `127.0.0.1` for AudioSocket:** the form refuses `::1` and other
  loopback addresses too. That's deliberately strict.

---

## 6. Developing and testing

Nothing is compiled, so the loop is copy, clear cache, reload:

```sh
A=fxs/luci-app-slic-fxs R=root@ROUTER
ssh $R mkdir -p /www/luci-static/resources/view/slic
scp -O $A/htdocs/luci-static/resources/view/slic/*.js $R:/www/luci-static/resources/view/slic/
scp -O $A/root/usr/share/luci/menu.d/*.json $R:/usr/share/luci/menu.d/
scp -O $A/root/usr/share/rpcd/acl.d/*.json $R:/usr/share/rpcd/acl.d/
scp -O $A/root/usr/share/rpcd/ucode/luci.slic $R:/usr/share/rpcd/ucode/
ssh $R 'rm -f /tmp/luci-indexcache*; rm -rf /tmp/luci-modulecache; killall -HUP rpcd'
ssh $R 'time ubus call luci.slic status'      # the backend on its own
```

**Offline test of the status page** (Node.js only, no router or browser):

```sh
node fxs/luci-app-slic-fxs/test/render_test.js
```

It runs `status.js` against a strict stand-in for LuCI's `E()` that fails on
any bare string child (§3, stored XSS). The input is `test/status-sample.json`,
a captured status with its addresses anonymised, plus attack strings
injected into every field that comes from outside. It prints the page as
text and exits non-zero on failure. Its own check was tested too: putting
back one bare string makes it fail. Run it after every change to
`status.js`. The package doesn't install `test/`.

**Traps:**
- **`ucode` regex escapes:** `\s` and `\d` work outside brackets only. Inside
  them `ucode` passes them to POSIX regex unchanged, so `[^\s;]` means "not a
  backslash, not an `s`, not `;`". Use `[^[:space:];]`. This cut the log path
  `/srv/...` to `/` during development.
- **No lazy quantifiers** (`.*?`) either: POSIX regex has none. Split on
  `/  +/` instead.
- **`logread -l N -e PATTERN`** counts the N lines *before* filtering, so it
  usually returns nothing. Filter first, then take the tail.
- **rpcd keeps the plugin loaded** between calls; the backend relies on this
  for its caches. `killall -HUP rpcd` makes rpcd re-execute and load new code.
- **Testing the init script's reload** needs the real path: `sh /etc/init.d/slic reload`
  only defines functions and exits (the `#!/bin/sh /etc/rc.common` line is
  what dispatches `reload`). A copy under another name would register a
  second procd service, because procd names it after the file.
- **Test with a limited login.** As root every permission check passes, so a
  missing ACL entry would never show. A LuCI login limited to `luci-base` +
  `luci-app-slic-fxs` (an rpcd `login` section) tests exactly what the ACL
  grants.

---

## 7. What was tested (2026-11-17)

| Part | How | Result |
|---|---|---|
| Backend | `ubus call luci.slic status` on the router | line state, last call and its events; Asterisk 23.1.0; five extensions with three phones registered (addresses, round trips); HEP on and loaded; 10 recent problems; no module errors |
| Active calls | a call file connecting a `Local` channel to the 600 echo test (nothing rings) | both channels listed: state, application, caller ID, duration, field for field as the raw output |
| Cost | `time ubus call …` ×3 | 1.9 s, then 0.44 s and 0.42 s |
| "Settings differ" detection | `uci commit` without reload | config 4, applied 3 |
| Save & Apply path | `uci commit` + the `config.change` event that rpcd sends | before the fix: no restart (§4). After: restart, new values in the daemon's environment |
| Enabled off/on | same path | daemon stopped at once, both modules stayed loaded; back on: running |
| JavaScript | parsed (`node`), served from the router | parses |
| Status page, rendered offline | `status.js` run in Node with a strict stand-in for LuCI's `E()`, fed the router's live status JSON plus attack strings (`<img src=x onerror=…>`) in caller ID, contact URI, endpoint name, channel, log lines and line events | page text as expected; **no string ever reached `E()` as a bare child**, so all 7 hostile strings stayed plain text (§3, security) |
| **The pages in a browser** | **not yet** | to do: open Services → Phone, check both tabs, and Save & Apply a change |
| Package | built with the SDK (`fxs/build.sh`, exit 0): the 5 packaged files are byte-identical to the repo; `apk` records the dependencies (`luci-base`, `rpcd-mod-ucode`, `slic-fxs`, three `ucode` modules) despite the SDK's "does not exist" warnings | — |
| Install | `apk add --allow-untrusted` of `slic-fxs` r6 and `luci-app-slic-fxs` r1 on the router (r5 files backed up first) | files owned by the packages; backend answers; a reload through the packaged init applied 3 → 4 and back |

---

## 8. Next pages

From the plan in order of usefulness:
- **Accounts:** add an extension, reset a password, show it once for setting up
  a phone. It must reuse `slic-asterisk-setup`'s code, not copy it.
- **Call history:** `Master.csv`.
- **SMS:** smstools3's spool.
- **A HEP on/off switch:** `scripts/router_hep.sh`'s logic.

A ubus `slic` object with events (doc 10 §10) would let the status page update
the moment the handset is lifted, instead of polling.

## How obtained

- LuCI and rpcd conventions read from the router itself:
  - `/usr/share/rpcd/ucode/luci` for the plugin shape, `popen` and quoting;
  - `luci-app-chrony`'s and `luci-base`'s ACLs (luci-base already allows
    `uci apply`/`confirm`);
  - `luci-app-nlbwmon`'s menu (an alias parent carrying the ACL).
- Versions: luci-base 26.315, rpcd 2026.10.04, ucode 2026.07.09,
  procd 2026.03.13.
- Output formats from `asterisk -rx` on the router, and timing with `time`.
- `ucode` behaviour probed with `ucode -e`: regex escapes, `split` on a regex,
  `stat().inode`, `localtime()` (1-based month, local time zone from
  `/etc/localtime`).
- The procd analysis:
  - `service/instance.c` (`instance_update`, `instance_config_changed`,
    `instance_restart`, `instance_timeout`) and `utils/utils.c`
    (`blobmsg_list_equal`) from procd's master branch;
  - the PID and `/proc/<pid>/environ` watched once a second after each
    change.
