# 13 — SMS gateway (smstools3) and logs that survive a reboot

Set up on the router on 2026-10-13. Everything marked *verified* was checked
live on the router. This doc is also the **rebuild guide**: if the router is
reset, §2.3 and §3.5 put everything back, and §4 proves it works.

## TL;DR

| What | Where it lives | Survives a reboot? | Verified |
|---|---|---|---|
| Received SMS | `/root/spool/sms/incoming/` (one file per message) | yes (USB extroot) | received, and pulled off the SIM after the fix |
| SMS to send | drop a file in `/root/spool/sms/outgoing/` | yes | test SMS delivered to a mobile |
| smsd config | `/etc/smsd.conf` | yes | — |
| System log (everything `logread` shows) | `/srv/log/messages` (+ `.old`), max ~2 MiB | yes | written; rotation tested |
| Asterisk log | `/srv/log/asterisk/messages.log` | yes | written |
| Call history (CDR) | `/srv/log/asterisk/cdr-csv/Master.csv` | yes | test call recorded, local time |
| Everything else in `/var` | RAM (`/var` → `/tmp`) | no, **on purpose** | — |

Not yet done: a real reboot test. Each path above is on the USB stick
(`/dev/sda1`, ext4, mounted as the overlay), so it should survive; §5 has the
one-line check for after the next reboot.

---

## 1. Two problems with the same root: things living in RAM

On OpenWrt, `/var` is a symlink to `/tmp`, which is RAM. That is the right
home for process IDs, locks and lease tables (§3.1), but it was also where
smstools3 put received SMS and where Asterisk put its log and call records.
So every reboot quietly deleted every received SMS and all call history.
Separately, SMS stopped arriving at all after a config edit (§2.2). Both are
fixed below.

---

## 2. The SMS gateway

### 2.1 How a received SMS travels

```
your phone ──network──▶ modem (Unisoc) ──stores it──▶ SIM card ("SM", 30 slots)
                                                         │
             smsd polls every few seconds over /dev/ttyUSB0 (AT commands)
                                                         ▼
                 reads it, writes /root/spool/sms/incoming/GSM1.xxxxxx,
                 then deletes it from the SIM (so the SIM never fills)
```

Two settings in smsd's `init2` line decide this chain:

- **`AT+CPMS="SM","SM","SM"`** chooses *which store* smsd reads from and
  where the modem saves new messages. On this modem the only store that works
  is the SIM.
- **`AT+CNMI=2,1,0,0,0`** tells the modem: store each new SMS and announce
  it, instead of pushing it straight down the serial line. With `2,2`
  ("route directly"), smsd still catches the message but logs *"you MUST
  DISABLE message routing"*, and a message that arrives while smsd is stopped
  is lost, because it was never stored anywhere.

### 2.2 What went wrong on 2026-10-13 (and the three traps)

1. **The modem's own memory has zero slots.** The first fix tried
   `CPMS="ME","ME","ME"` (modem memory). The modem accepts the command, but
   `AT+CPMS?` answers `"ME",0,0`: zero used, **zero capacity**. New SMS
   silently went to the SIM, while smsd checked the empty modem memory every few
   seconds and found nothing. No error appeared anywhere: the messages
   were sitting on the SIM the whole time. Use `"SM"`.
2. **One stray `+` disables the whole line.** `init2 = AT++CPMS=...` makes
   the modem reject the entire line, and smsd logs *"Modem did not accept the
   second init string, Operation not supported"*. Each command was fine
   on its own.
3. **The storage selection is shared by every serial port.** Reading the SIM
   from `ttyUSB3` with `AT+CPMS="SM"` and then `AT+CMGL=4` as *two* commands
   listed the empty `ME` store: smsd (on `ttyUSB0`) had reset the selection in
   between. From a second port, always send it as one command:
   `AT+CPMS="SM";+CMGL=4`.

Also: `AT+CMGF=1` (text mode) in `init2` does nothing useful. smsd works in
PDU mode, and sending worked with that line still present, so smsd sets the
mode itself (inferred from that, not traced).

### 2.3 Rebuild from nothing

```sh
apk update && apk add smstools3 sms-tool
mkdir -p /root/spool/sms/incoming /root/spool/sms/outgoing \
         /root/spool/sms/checked /root/spool/sms/failed /root/spool/sms/sent
```

`/etc/smsd.conf` (the whole working file, comments trimmed):

```ini
devices = GSM1
incoming = /root/spool/sms/incoming
outgoing = /root/spool/sms/outgoing
checked = /root/spool/sms/checked
failed = /root/spool/sms/failed
sent = /root/spool/sms/sent
receive_before_send = no
autosplit = 3
logfile = 1          # 1 = send smsd's log to syslog (logread), not a file
loglevel = 5

[GSM1]
device = /dev/ttyUSB0
incoming = yes
baudrate = 115200
init = AT
init2 = AT+CPMS="SM","SM","SM";+CNMI=2,1,0,0,0
keep_open = yes
```

```sh
/etc/init.d/smstools3 enable
/etc/init.d/smstools3 restart
logread -e smsd -f        # expect "IMEI: …", "IMSI: …", then nothing alarming
```

The package's init script still creates `/var/spool/sms/*` at every start.
That's harmless: smsd uses the paths from the config.

### 2.4 Everyday use

| Task | How |
|---|---|
| Read received SMS | `ls -lt /root/spool/sms/incoming/`, then `cat` a file. Headers `From:`, `Sent:`, then a blank line, then the text |
| Send an SMS | `printf 'To: 947XXXXXXXX\n\nHello\n' > /tmp/m && mv /tmp/m /root/spool/sms/outgoing/msg1` (international format without `+`; write elsewhere and `mv`, so smsd never reads a half-written file) |
| Did it go? | the file moves to `sent/` (or `failed/`); `logread -e smsd` shows `SMS sent, Message_id: …` |
| What's on the SIM right now | `sms_tool -d /dev/ttyUSB3 at 'AT+CPMS="SM";+CMGL=4'` (`+CPMS: n,30,…` = n stored of 30) |
| Watch live | `logread -e smsd -f` |

`ttyUSB3` also speaks AT and is free while smsd holds `ttyUSB0`
(`keep_open = yes`). Read-only queries there are safe. Don't delete
messages from it while smsd is running.

### 2.5 Things that can still bite

- **ModemManager** is installed and lists `ttyUSB0` as its primary port. It's
  idle now (state `disabled`, holds no port). If anything enables the modem
  in ModemManager (`mmcli -m 0 -e`, a LuCI page), both programs would talk on
  the same port, and ModemManager can take incoming SMS for itself. Keep it
  disabled, or point smsd at a port ModemManager doesn't use.
- **The SIM holds 30.** smsd empties it, so this only matters if smsd is
  stopped for a long time while messages arrive. A full SIM makes the
  network hold and then drop new messages.
- **`incoming/` is never cleaned.** It holds about 250 bytes per message, so
  this isn't urgent, but nothing deletes old ones.

---

## 3. Logs that survive a reboot

### 3.1 Why not move all of `/var` to the USB stick

Most of `/var` is state that *must* start empty at boot:

| Path | What | Persist? |
|---|---|---|
| `/var/run`, `/var/lock`, `/var/state` | process IDs, sockets, locks | **no**: a stale PID or lock after a reboot makes a service think it's already running, or makes a tool wait forever |
| `/var/resolv.conf*`, `/var/hosts`, `/var/etc` | generated by netifd/dnsmasq at every boot | no |
| DHCP leases, `/var/cache` | rewritten constantly | no (USB wear for nothing) |
| `/var/lib/asterisk/astdb.sqlite3` | which softphones are registered, any forward/DND settings | optional; losing it just means phones register again |
| **received SMS, call history, logs** | data you'd want after a crash | **yes**: moved (below) |

So only the data moves. Logs go through **one** mechanism (the system log)
instead of each program writing its own file.

### 3.2 System log → `/srv/log/messages`

OpenWrt's log daemon keeps a RAM ring buffer (what `logread` shows). Setting
`log_file` adds a second process, `logread -f -F <file> -S <size>`, that
appends every line to a file.

```sh
mkdir -p /srv/log && chmod 755 /srv /srv/log
uci set system.@system[0].log_file='/srv/log/messages'
uci set system.@system[0].log_size='1024'       # KiB
uci commit system && /etc/init.d/log restart
```

- **`log_size` is in KiB** and does two jobs. It sizes the RAM buffer (it
  was 128), and it is the point where the file rotates. *Verified* with a
  2 KiB test: past the limit the file is renamed to `messages.old` and a new
  one starts, keeping one old generation. So the disk use is at most about
  **2 × 1 MiB**.
- **Restarting the log service empties the RAM buffer.** The history
  `logread` showed before the restart is gone; from then on it's also in
  the file.
- **The extroot trap.** `/srv/log` is on the USB stick only because the USB
  stick *is* the root filesystem (extroot). If the stick ever fails to mount,
  the router boots from the internal overlay, a **3.6 MiB** partition,
  and the log goes there. The 1 MiB cap is what keeps that from filling the
  internal flash, so don't raise it carelessly or set it to 0. The init
  script's "wait for the mount" check doesn't help here, because the path
  isn't on a separate mount.
- Wear: a home router logs kilobytes to a few MB a day. That's nothing for a
  7 GB stick.

### 3.3 Asterisk's log and call history → `/srv/log/asterisk`

Three separate things had to be true, and only the first was obvious.

1. **Point `astlogdir` at the new folder**, in `/etc/asterisk/asterisk.conf`.
2. **The trap: the `[directories](!)` section is ignored.** In the stock
   file the section header carries `(!)`, which marks it as a *template*.
   Asterisk ignores it and uses its built-in paths, so editing `astlogdir`
   alone changed nothing (`core show settings` still said
   `/var/log/asterisk`). The fix is to remove `(!)`, which makes every line in
   the section live. All of them already matched the built-in values except
   `astkeydir` (IAX2 keys, unused), which is commented out so it stays at the
   built-in `/usr/share/asterisk/keys`.
3. **Asterisk wasn't recording calls at all.** `cdr show status` said
   logging was enabled, but no backend was installed. The engine counted
   calls ("2 calls processed") and wrote nothing. Installing
   `asterisk-cdr-csv` adds the CSV writer (`[csv]` section in `cdr.conf`).

Then two details:

- **Timestamps were in UTC** even with `usegmtime=no`. Asterisk ignores
  OpenWrt's short time-zone string (`<+0530>-5:30` in `/etc/TZ`) and only
  reads a real zone file via `/etc/localtime`. That link pointed nowhere,
  because no zone database was installed. **`apk add zoneinfo-asia`** (plus
  `zoneinfo-core`, pulled in automatically), then `/etc/init.d/system reload`,
  makes `/tmp/localtime → /usr/share/zoneinfo/Asia/Colombo`, and the call
  records show local time. (If `apk add` says *ADB integrity error*, the
  package list is stale: `apk update` first.)
- **Permissions:** Asterisk runs as user `asterisk`, and `/root` is
  `drwxr-x---` (closed to others). That's why logs live in `/srv/log`, not
  `/root/log`. SMS can stay in `/root` because smsd runs as root.

### 3.4 Reading the call history

`/srv/log/asterisk/cdr-csv/Master.csv`, one line per call leg. Columns, in
order: account code, source number, destination, context, caller ID, channel,
destination channel, last application, its argument, **start, answer, end**
(local time), **duration, billed seconds**, disposition (`ANSWERED`,
`NO ANSWER`, `BUSY`), flags, unique ID, user field.

```sh
cut -d, -f2,3,10,14,15 /srv/log/asterisk/cdr-csv/Master.csv | tail
```

Unanswered calls are not logged (`Log unanswered calls: No` in
`cdr show status`; set `unanswered = yes` under `[general]` in `cdr.conf` if
you want them).

### 3.5 Rebuild from nothing (after Asterisk is installed per doc 12)

```sh
apk update && apk add asterisk-cdr-csv zoneinfo-asia
/etc/init.d/system reload                       # links /tmp/localtime
mkdir -p /srv/log/asterisk/cdr-csv
chown -R asterisk:asterisk /srv/log/asterisk && chmod 750 /srv/log/asterisk
sed -i -e '1s/^\[directories\](!)/[directories]/' \
       -e 's#^astlogdir => .*#astlogdir => /srv/log/asterisk#' \
       -e 's#^astkeydir => #;astkeydir => #' /etc/asterisk/asterisk.conf
sed -i 's/^usegmtime=yes/usegmtime=no/' /etc/asterisk/cdr.conf
/etc/init.d/asterisk restart
asterisk -rx 'core show settings' | grep 'Log directory'   # → /srv/log/asterisk
asterisk -rx 'module show like cdr'                         # → cdr_csv.so Running
```

`slic-asterisk-setup` doesn't touch `asterisk.conf` or `cdr.conf`, so
re-running it keeps these changes. **After an Asterisk package upgrade**,
re-check the `Log directory` line: a new stock `asterisk.conf` would bring
`(!)` back.

---

## 4. Verify checklist

| Check | Command | Expect |
|---|---|---|
| smsd reads the right store | `logread -e smsd \| tail` | no "did not accept the second init string" |
| SIM is being emptied | `sms_tool -d /dev/ttyUSB3 at 'AT+CPMS="SM";+CMGL=4'` | `+CPMS: 0,30,…` after smsd has run |
| SMS receive | text the SIM's number from a mobile | a new file in `/root/spool/sms/incoming/` within ~10 s |
| SMS send | §2.4 | file in `sent/`, message on the phone |
| System log to disk | `logger -t test hello; tail -1 /srv/log/messages` | the `hello` line |
| Asterisk log dir | `asterisk -rx 'core show settings' \| grep 'Log directory'` | `/srv/log/asterisk` |
| Call records | make a call, then `tail -1 /srv/log/asterisk/cdr-csv/Master.csv` | a line with local-time timestamps |
| **Survives reboot** | `reboot`, then `head -3 /srv/log/messages; ls /root/spool/sms/incoming` | lines and messages from *before* the reboot |

## 5. Open items

- **Reboot test not done yet** (last row above). Everything is on the USB
  root filesystem, so it should hold, but that's inferred, not seen.
- **Asterisk's `messages.log` doesn't rotate by itself.** Asterisk only
  rotates on `logger rotate`. At `notice,warning,error` level it grows slowly
  (a few KB in two days here), so this is not urgent. A weekly cron
  `asterisk -rx 'logger rotate'` would cap it.
- `Master.csv` and `incoming/` grow forever (a few hundred bytes per call or
  SMS).
- ModemManager vs smsd (§2.5): fine while ModemManager stays disabled.

## How obtained

All on the router on 2026-10-13, over ssh:

- **SMS:**
  - `AT+CPMS=?` → `("ME","SM")`;
  - `AT+CPMS?` → `"ME",0,0` (zero capacity);
  - after `AT+CPMS="SM","SM","SM"` → `+CPMS: 2,30,…`;
  - `AT+CMGL=4` listed the two "missing" messages, which were decoded and
    then backed up to `/root/sms-sim-backup-20261013.txt`.
- **smsd log after the fix:** two `SMS received` lines at 23:54:32 and 23:54:33. The SIM then showed `0,30`.
- **Send test:** smsd logged `SMS sent, Message_id: 1 … sending time 4 sec` (23:40:31), and the message arrived on the phone.
- **Rotation test:**
  - `logread -f -F /tmp/lt -S 2` with 60 `logger` lines → `lt` (1552 B) + `lt.old` (2134 B).
- **Asterisk:**
  - `core show settings` before the change: `/var/log/asterisk`, unchanged after editing `astlogdir`;
  - after removing `(!)`: `/srv/log/asterisk`;
  - `module show like cdr` showed only the engine until `asterisk-cdr-csv` was installed;
  - a call file to `Local/601@house` produced CDR lines stamped 18:26 (UTC), then 23:58 after `zoneinfo-asia`.
- **Backups of the original files:** `/root/backups/persist-20261013/` (`smsd.conf`, `system`, `asterisk.conf`, `cdr.conf`).
