# 12 — Home PBX: setup, configuration and connecting devices

Written 2026-10-11. The hands-on companion to
[11-pbx-asterisk.md](11-pbx-asterisk.md): doc 11 explains *how it works*,
this one is *what to type*. It covers installing the PBX from nothing,
every setting you're likely to change and where it lives, connecting
softphones, desk phones and programs, and what to check when something
doesn't work. §9 records exactly how it was set up on this router.

You need the phone port working first (doc 09 §10: the `slic-fxs` and two
kmod packages installed, dial tone on lifting the handset).

---

## 1. What you end up with

| Number | Rings / does |
|---|---|
| **100** | the analog phone on the RJ11 jack |
| **201, 202, 203** | SIP logins for LAN devices (more on request, §4.2) |
| **600** | echo test |
| **601** | "hello world" announcement |
| **800** | your program (AudioSocket app, §5.4) |

Anyone can call anyone: the handset dials `201#` to ring a softphone, a
softphone dials `100` to ring the handset. Nothing is reachable from the
internet (§4.7).

---

## 2. Install from nothing

On the router, as root:

```sh
apk update                                  # refresh the package lists first
APP=192.168.1.20:9092 slic-asterisk-setup   # APP = the PC that will run your app (optional)
```

What happens and how long it takes:

1. `apk add` installs 32 Asterisk packages (about 5.5 MB on disk) from the
   official OpenWrt telephony feed; it depends on your connection (not timed).
2. It finds the router's LAN address from the firewall zone named `lan`.
3. It writes `/etc/asterisk/pjsip.conf`, `extensions.conf`, `rtp.conf`,
   selects Sri Lanka's call-progress tones, and creates the three SIP
   logins with random 20-character passwords.
4. It starts Asterisk (and enables it at boot), restarts the phone daemon,
   and prints:

```
SIP server: 192.168.1.254 port 5060 (UDP or TCP), LAN only. Accounts:
  user 201  password …
  user 202  password …
  user 203  password …
Dial 100 for the RJ11 phone, 600 echo test, 601 hello-world, 800 AudioSocket app (…).
```

Those passwords are the only copy you'll be shown; they stay in
`/etc/asterisk/pjsip_secrets.conf` (§4.2 shows how to read one again).

**Check it worked** (each should succeed):

```sh
pidof asterisk                                   # a process id
netstat -lnup | grep 5060                        # bound to the LAN address only
asterisk -rx "pjsip show endpoints"              # 201 202 203 listed
ls -l /var/run/slic.ctl                          # prw--w---- root asterisk
```

Then lift the handset and dial **601** (wait 3 s, or press **#**): you
should hear "hello world … goodbye", then a busy tone.

The script is safe to re-run at any time: it keeps your passwords, your
app address and your own files, and saves any config file it replaces as
`*.bak-DATE`.

---

## 3. Everyday commands

| Want to | Command (on the router) |
|---|---|
| Is it running? | `/etc/init.d/asterisk status`, `/etc/init.d/slic status` |
| Restart | `/etc/init.d/asterisk restart` (drops calls in progress) |
| Who is logged in? | `asterisk -rx "pjsip show contacts"` |
| Calls in progress | `asterisk -rx "core show channels"` |
| Live console (Ctrl-C to leave; Asterisk keeps running) | `asterisk -rvvv` |
| Phone line state | `cat /var/run/slic.state` |
| Phone daemon log (digits heard, calls, drops) | `logread -e slicctl` |
| Asterisk log | `tail -f /var/log/asterisk/messages.log` |
| Reload the dialplan after editing it | `asterisk -rx "dialplan reload"` |
| Reload SIP settings | `asterisk -rx "module reload res_pjsip.so"` |
| Ring the handset from the router (test) | the call-file recipe below |

**Two log traps:** Asterisk's log timestamps are **UTC** (this OpenWrt has no
time-zone data for Asterisk; `logread` shows local time), and
`/var/log` lives in RAM, so the Asterisk log is gone after a reboot.

Ring the handset with a test caller ID and connect the answer to the echo
test (pick up, hear yourself):

```sh
f=/var/spool/asterisk/tmp/test.call
printf 'Channel: Local/100@house\nCallerID: "Test Call" <0112345678>\nContext: house\nExtension: 600\nPriority: 1\nMaxRetries: 0\nWaitTime: 50\n' > $f
chown asterisk:asterisk $f && mv $f /var/spool/asterisk/outgoing/
```

(`channel originate` in the console doesn't exist here: its module isn't
packaged. Call files do the same job. Never put a `;` in one: it starts a
comment.)

---

## 4. Configuration

### 4.1 Where everything lives

The rule: **files the setup writes, it may rewrite; files you own, it never
touches.** If you edit a setup-written file by hand, your edit lasts until
the next `slic-asterisk-setup` run (the old file is kept as `*.bak-DATE`).

| File | Written by | Holds |
|---|---|---|
| `/etc/config/slic` | you (uci) | the phone line: AudioSocket address, dialling timeout, caller-ID format |
| `/etc/asterisk/pjsip.conf` | setup | SIP transports (LAN address), the LAN-only ACL, account templates |
| `/etc/asterisk/pjsip_accounts.conf` | setup | one block per SIP login (generated from the secrets file) |
| `/etc/asterisk/pjsip_secrets.conf` | setup, keeps existing passwords | the passwords; mode 0600, **never copy it into git** |
| `/etc/asterisk/extensions.conf` | setup | the numbering plan (dialplan) |
| `/etc/asterisk/rtp.conf` | setup | audio ports 10000–10199 |
| `/etc/asterisk/indications.conf` | setup adds `[lk]` and sets `country=` | ringback / busy / congestion tones |
| **`/etc/asterisk/extensions_local.conf`** | **you** | your own numbers (§4.5) |
| **`/etc/asterisk/pjsip_local.conf`** | **you** | your own SIP settings (§4.6) |
| `/usr/share/slic/asterisk/` | the `slic-fxs` package | the templates the setup copies from |

### 4.2 SIP logins (accounts)

Logins are numbers from **200 to 299** (the dialplan routes `2XX` to them).

```sh
# show one account's password
grep -A3 '201-auth' /etc/asterisk/pjsip_secrets.conf

# add accounts 204 and 205 (new random passwords; others unchanged)
ACCOUNTS="204 205" slic-asterisk-setup --no-apk

# remove 205: delete its block, then re-run
vi /etc/asterisk/pjsip_secrets.conf      # delete [205-auth] … password=… lines
slic-asterisk-setup --no-apk

# new passwords for everyone (every device must be updated!)
slic-asterisk-setup --no-apk --new-passwords
```

Each login allows **2 devices at once** (`max_contacts=2`), e.g. a phone
and a laptop with the same account; both ring. Caller ID for an account is
"Ext 2xx"; the handset shows it on incoming calls.

### 4.3 The phone line (`/etc/config/slic`)

```sh
uci set slic.main.dial_timeout=4; uci commit slic; /etc/init.d/slic restart
```

| Option | Default | What it does | Recommendation |
|---|---|---|---|
| `dial_timeout` | `3` | seconds without a key press that end dialling; `#` dials at once | 3; raise to 4–5 if you dial slowly and get cut off |
| `cid_format` | `bell` | caller ID: `bell` (after 1st ring), `v23`, `dtmf` | `bell`: the only one confirmed on the Uniden |
| `audiosocket` | `127.0.0.1:4573` | where the daemon takes Asterisk's audio connections; `off` = no calls | leave it; never put it on a LAN address |
| `clk_switch`, `spi_hz`, `enabled` | | hardware (doc 10 §3.4) | leave |

### 4.4 Numbers and timing in `extensions.conf`

The `[globals]` block at the top:

| Setting | Default | Meaning |
|---|---|---|
| `PHONE_RING_SECS` | `45` | how long the handset rings before the caller gets "no answer" |
| `AUDIOSOCKET_APP` | from `APP=` | where 800 sends calls; change with `APP=host:port slic-asterisk-setup --no-apk` |
| `SLIC_*` | | how Asterisk reaches the phone daemon; don't change |

A SIP extension rings for **30 s** (`Dial(PJSIP/${EXTEN},30,…)`). To change
either time permanently, edit `/usr/share/slic/asterisk/extensions.conf`
too, or the next setup run puts it back (and a package upgrade replaces
that template; keep a note).

### 4.5 Your own numbers (`extensions_local.conf`)

Lines at the top of this file join the `[house-local]` context, which every
caller (handset and SIP) can reach. Built-in numbers win over yours if both
exist. After editing: `asterisk -rx "dialplan reload"`.

```
; play a beep (tested 2026-10-11)
exten => 699,1,Answer()
 same => n,Playback(beep)
 same => n,Hangup()

; short code: 9 rings account 201
exten => 9,1,Goto(house,201,1)

; a second app on another PC (same pattern as 800)
exten => 801,1,Answer()
 same => n,Set(UUID=${SHELL(cat /proc/sys/kernel/random/uuid):0:36})
 same => n,AudioSocket(${UUID},192.168.1.50:9093)
 same => n,Hangup()
```

Only `699` was tried live; the other two use the same building blocks as
the built-in numbers. On the handset, a one-digit number like `9` dials
after the 3 s pause, or press `9#`.

**Watch out:** the handset can't dial numbers containing letters, and
`System()`/`SHELL()` run as user `asterisk` on the router: never pass
caller-supplied text to them without `FILTER()` (doc 11 §4).

### 4.6 Your own SIP settings (`pjsip_local.conf`)

For anything the generated accounts don't cover. Example: a device that
only does G.711 and wants a fixed caller name:

```
[210](lan-endpoint)
auth=210-auth
aors=210
allow=!all,ulaw
callerid="Kitchen" <210>
[210](lan-aor)

[210-auth]
type=auth
auth_type=userpass
username=210
password=CHANGE-ME-to-something-long
```

Then `asterisk -rx "module reload res_pjsip.so"`. Keep the file private
(`chmod 600`, `chown asterisk:asterisk`) if it holds passwords. Don't reuse
a number the setup generates (201–203 or any in `ACCOUNTS`).

### 4.7 What keeps it off the internet (don't undo these)

| Guard | Where |
|---|---|
| SIP listens on the LAN address only | `pjsip.conf` transports |
| SIP requests only from the LAN subnet and the router itself | `[lan-only]` ACL in `pjsip.conf` |
| The firewall rejects all input from the internet | firewall zone `wan` (`input REJECT`) |
| Every device must log in with a 20-character password | `pjsip_secrets.conf` |

**Never port-forward 5060 or 10000–10199.** The audio ports listen on all
addresses; the firewall is the only thing keeping them off the WAN. To use
the PBX away from home, the safe route is a VPN into the LAN (e.g.
WireGuard), not a port forward. Not set up yet.

### 4.8 Tones

```sh
TONES=uk slic-asterisk-setup --no-apk    # any [xx] section in indications.conf
```

Default `lk`: Sri Lanka's values from ITU's E.180 tone list (doc 11 §2).
They're what SIP callers and the handset hear as ringback and busy.
The handset's dial and busy tones come from the phone daemon, which uses
Sri Lanka's values whatever `TONES` says (425 Hz; `tone_frame()` in
`slicctl.c`).

---

## 5. Connecting devices

### 5.1 A softphone on a mobile phone

Apps: **Linphone** (free, open source) or **Zoiper** (free tier), Android
or iOS. The phone must be on your home Wi-Fi (the same LAN, not a guest
network: the ACL only admits the LAN subnet).

| Field (names vary by app) | Value |
|---|---|
| Username / SIP user / extension | `201` |
| Password | from `grep -A3 '201-auth' /etc/asterisk/pjsip_secrets.conf` |
| Domain / server / registrar | the router's LAN address, e.g. `192.168.1.254` |
| Port | `5060` |
| Transport | **UDP** (TCP also works) |
| Outbound proxy, STUN, ICE, TURN | **off / empty** (it's all on one LAN) |
| Codecs | G.722, PCMU (μ-law), PCMA (a-law); others off |
| DTMF | RFC 2833 / RFC 4733 (sometimes "out-of-band") |
| Encryption (SRTP/TLS) | off (not configured on the PBX) |

**Check:** on the router, `asterisk -rx "pjsip show contacts"` shows
`201/sip:201@<phone's IP>… Avail`. Then dial `100` from the app: the handset
rings and shows "201". From the handset, dial `201#`: the app rings.
*Verified 2026-10-11 with a softphone on a mobile over Wi-Fi: both
directions, two-way audio.*

Mobile apps may sleep: an app that isn't kept running may not ring for
incoming calls (wake-up push notifications need the app maker's cloud
service; not set up). Calling out always works.

### 5.2 A softphone on a PC

Same settings (Linphone desktop, MicroSIP on Windows, Zoiper). If calls
*from* the PC work but calls *to* it don't ring, the PC's firewall is
dropping the router's INVITE. On Ubuntu with ufw:
`sudo ufw allow from <router LAN IP> proto udp` (inferred, not yet tested
on this setup).

### 5.3 A desk IP phone or an analog phone adapter (ATA)

Same fields as §5.1, in the device's web page (often "Account 1": SIP
server, SIP user ID, authenticate ID = same number, password). Turn off
NAT traversal / STUN. Not tested yet; any standard SIP device should work.

### 5.4 A program or an AI agent

Two ways in:

| Way | When | How |
|---|---|---|
| **AudioSocket via 800** | you want raw audio and no SIP stack | listen on TCP, run `APP=yourhost:port slic-asterisk-setup --no-apk`, dial 800. Protocol and traps in doc 11 §8; template `fxs/tools/audiosocket_demo.py` |
| **As a SIP account** | your program should call out, be called by number, or get clean keypress events | give it an account (`ACCOUNTS=204 …`); it behaves like a softphone. Template `fxs/tools/sip_bot.py` (below) |

**AudioSocket sample** (beep, then your voice back 1 s later):

```sh
python3 fxs/tools/audiosocket_demo.py --listen 0.0.0.0:9092
# then dial 800 on the handset
```

**SIP sample: `fxs/tools/sip_bot.py`.** A complete SIP phone in one Python
file with no dependencies (Python 3.10+; works on 3.14, which dropped the
`audioop` module that older pure-Python SIP libraries need). It does what
any SIP app does, so it's the place to read how the pieces fit: log in
(REGISTER with the password challenge), answer or place calls (INVITE /
ACK / BYE), describe its audio (SDP), send and receive 20 ms G.711 audio
packets (RTP), and read keypresses (RFC 4733).

```sh
export SIP_PASSWORD=…          # grep -A3 '202-auth' /etc/asterisk/pjsip_secrets.conf
# answering machine: beep, parrot, records each call to sip-<id>.wav;
# keypad presses are printed and '#' hangs up
python3 fxs/tools/sip_bot.py --server 192.168.1.254 --user 202 listen
# notifier: ring the RJ11 phone, play a chime (or --play msg.wav), hang up
python3 fxs/tools/sip_bot.py --server 192.168.1.254 --user 202 call 100
python3 fxs/tools/sip_bot.py --server 192.168.1.254 --user 202 call 100 --play msg.wav
```

*Verified 2026-10-11:* logged in as 202; called 601 and recorded "hello
world … goodbye"; called `601#`; answered a call from the router and
recorded it. Not yet tried: `call 100` (rings the handset).

What each sample is a starting point for:

| Idea | Start from | Add |
|---|---|---|
| doorbell / server alert rings the phone | `sip_bot.py call 100 --play alert.wav` | text-to-speech to make the WAV (e.g. `espeak-ng -w out.wav` then convert to 8 kHz) |
| answering machine / voicemail | `sip_bot.py listen` | play a greeting instead of the parrot; it already records |
| phone menu ("press 1 for…") | `sip_bot.py listen` | act on the printed keys (SIP callers; the RJ11 handset's keys travel as tones, doc 11 §6) |
| AI voice agent | either sample | speech-to-text on what arrives, a model, text-to-speech into the outgoing buffer; 8 kHz audio (doc 11 §8.1); echo (doc 11 §7) |

For something bigger than a sample, use a maintained SIP stack instead of
growing this one (none of these was tried with this PBX yet):
**pjsua2** (PJSIP's C++/Python API, the same SIP library Asterisk uses),
**baresip** (small command-line SIP client, scriptable), **liblinphone**
(the engine inside Linphone). Voice-AI frameworks with a SIP connector can
log in as an account the same way.

### 5.5 Something on the other side of the router (a different subnet)

Only the LAN subnet is admitted. A device on another VLAN or a guest Wi-Fi
is rejected before it can log in. Widening that means adding a `permit=`
line to `[lan-only]` in `pjsip.conf` (and re-applying it after each setup
run) plus a firewall rule; think about who else is on that network first.

---

## 6. Test checklist (after installing or changing things)

| # | Do | Expect |
|---|---|---|
| 1 | lift the handset | dial tone |
| 2 | dial `601` | "hello world … goodbye", then busy tone (425 Hz, 0.75 s on/off) |
| 3 | dial `600#` | instructions, then your own voice |
| 4 | test call (§3) | handset rings, "Test Call 0112345678" shown; answer = echo test |
| 5 | test call, don't answer, set `WaitTime: 8` | ringing stops after ~8 s |
| 6 | softphone dials `100` | handset rings with "201" |
| 7 | handset dials `201#` | softphone rings; handset hears the double "brr-brr" ringback |
| 8 | handset dials `800#` with your app running | your app answers |

After each, `logread -e slicctl | tail` should end with `0 frames dropped`.

---

## 7. When something doesn't work

| Symptom | Likely cause | Check / fix |
|---|---|---|
| Dial tone, but any number gives busy tone at once | Asterisk not running, or the dial hook failed | `logread -e slicctl \| tail` (`dial hook failed`, `cannot place a call`); `pidof asterisk` |
| Digits not heard (log shows no `digit` lines) | phone in pulse mode; very weak keypad | set the phone to tone (DTMF) dialling; doc 10 §8 |
| Number dialled too early | you paused > 3 s | dial faster, end with `#`, or raise `dial_timeout` |
| "That is not a valid extension" | number not in the plan | §1, §4.5 |
| You hear your own voice faintly on a softphone | line echo from the analog phone side; the driver cancels it since kmod r3. Check `ec=Y` in `/sys/module/mt7621_pcm_raw/parameters/` | doc 11 §7.1 |
| Calls to 100 get busy at once | handset off-hook or line not idle; FIFO permissions | `cat /var/run/slic.state`; `ls -l /var/run/slic.ctl` |
| Softphone: "registration failed", 401/403 | wrong password or username; device not on the LAN subnet | `tail /var/log/asterisk/messages.log` (`Failed to authenticate` = password; `Not match Endpoint ACL` = wrong network) |
| Softphone registered, but no audio | codec mismatch, or a firewall on the device | enable PCMU/PCMA; `asterisk -rvvv` during the call |
| Softphone doesn't ring on incoming calls | app asleep (mobile), PC firewall | keep the app open; §5.2 |
| One-way audio between softphone and handset | the device sends to a wrong address (STUN on) | turn STUN/ICE off |
| Extension 800: call drops after ~2 s | your app isn't sending audio continuously | send silence frames every 20 ms (doc 11 §8.1) |
| Softphone: number ending in `#` rejected | (fixed 2026-10-11) softphones send `#` as part of the number | `[from-lan]` now drops a trailing `#` |
| Log: "no local system nameservers" | harmless | — |
| Log: "The canary is no more" | harmless: Asterisk lowered its priority | — |

---

## 8. Backup, upgrade, removal

**Back up** (contains passwords, keep it private):

```sh
tar czf /root/backups/asterisk-$(date +%Y%m%d).tgz /etc/asterisk /etc/config/slic
```

**Upgrade the phone package** (new `slic-fxs` build): `apk add
--allow-untrusted /tmp/slic-fxs-*.apk`. This updates the daemon, the
templates and the setup script; your `/etc/asterisk` is untouched until you
re-run `slic-asterisk-setup --no-apk` to apply new templates.

**After an OpenWrt upgrade**: rebuild the kmods for the new kernel (doc 09
§10), `apk update`, then re-run `slic-asterisk-setup` (reinstalls the
Asterisk packages if the upgrade dropped them).

**Turn the PBX off** (phone port keeps working as before, without dialling
out): `/etc/init.d/asterisk stop; /etc/init.d/asterisk disable`.

**Remove it**: stop and disable as above, then `apk del` the `asterisk*`
packages (list them with `apk list -I | grep ^asterisk`). `/etc/asterisk`
stays behind; delete it yourself if you want the passwords gone.

---

## 9. How it was set up on this router (2026-10-11)

For the record, in order:

1. **Checked the ground:** OpenWrt 25.12.5 with apk v3; Asterisk 23.1.0 in
   the `telephony` feed; 248 MB RAM (135 MB free); 7 GB USB extroot.
   `voip_br` (the ISP's VoIP VLAN) **left disabled** on purpose.
2. **Backed up** the stock `/etc/asterisk` to
   `/root/backups/etc-asterisk-stock-20261011.tgz` (the stock files also
   stay as `*.stock`).
3. **Installed** the 32 Asterisk packages with `apk add` (first without
   `codec-gsm`: the bundled sounds then failed to play, so it was added).
4. **Wrote and tested the config by hand**, then turned it into the
   templates and `slic-asterisk-setup`. Along the way: the first password
   generator used `base64`, which BusyBox lacks, and produced empty
   passwords (caught before anything could log in); `acl=` on an endpoint
   rejected every request (it names `acl.conf` entries, so a global
   `type=acl` is used instead).
5. **Tested SIP from the PC** with `sipp`: called 601 as account 202
   (password challenge passed, announcement played).
6. **Rebuilt the phone packages** (kmods r2, `slic-fxs` r2) with the
   AudioSocket bridge and DTMF detector; installed with `apk add
   --allow-untrusted`; ran `APP=<PC>:9092 slic-asterisk-setup`.
7. **Live tests on the handset** with the owner: 601 (first attempt failed:
   BusyBox `pgrep -x` broke the dial hook; fixed with `pidof`), 600, an
   incoming test call with caller ID, 800 to the demo app on the PC, a
   caller hanging up mid-ring (found `ExecIf` missing; fixed with
   `GotoIf`).
8. **Softphone** (account 201, mobile on Wi-Fi): called 100, and the
   handset called 201. Both worked first time.
9. **Tones:** the ringback was the US one; switched to Sri Lanka's from
   ITU's E.180 table (`slic-fxs` r3, which also made accounts a list you can
   extend and remembers the app address).

## How obtained

Every command above was run on the router on 2026-10-11 unless marked
"not tested"; outputs quoted are from those runs (addresses changed to
examples).
