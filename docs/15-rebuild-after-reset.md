# 15 — Rebuilding the router after a factory reset

One runbook for the whole router, from "just reset" to "PBX, firewall and
packages back". It ties together scripts written separately:

| Step | Script | Doc |
|---|---|---|
| Extroot (the USB stick as the writable root) | [`scripts/router_extroot.sh`](../scripts/router_extroot.sh) | this doc |
| Packages added on top of the firmware | [`scripts/router_packages.sh`](../scripts/router_packages.sh) + [`scripts/router-packages.txt`](../scripts/router-packages.txt) | this doc |
| PBX: Asterisk modules, configs, accounts | `slic-asterisk-setup` (in `slic-fxs`) | 12 §8.1 |
| Firewall, SSH, nginx hardening | [`scripts/router_fw_provision.sh`](../scripts/router_fw_provision.sh), then the audit | 14 |

Written 2026-11-17. **Each script was tested on the live router or on
stand-in folders, but the full sequence has not been run end to end**,
because that takes a real reset (§3 says what was tested how).

---

## 1. Why this order

The order is a chain: each step needs the one before it.

1. **Extroot comes first, because the router has almost no room of its own.**
   The flash holds a 12.5 MiB read-only firmware image, 100% full
   (`rootfs`, squashfs), and **4.1 MiB of writable space** (`rootfs_data`,
   jffs2; 3.8 MiB free when empty). The packages in the list take about
   250 MiB. Installing anything before the stick is mounted as the overlay
   fills the internal flash within the first few packages. The firmware
   already contains `block-mount`, `kmod-usb-storage`, `kmod-fs-ext4` and
   `e2fsprogs`, so attaching the stick needs configuration only.
2. **The LuCI backup comes second, because it brings the network.** It
   restores the PPPoE login, the LAN address `192.168.1.254`, the SSH key
   and the root password. Without the WAN, apk can't download anything.
   Restoring it *before* the packages leaves `/etc/asterisk` at mode `0700`,
   the 2026-11-05 outage (doc 12 §8.1). `slic-asterisk-setup` from `slic-fxs`
   r6 repairs that in step 6, so this order is safe from r6 on.
3. **Packages third:** the feed packages, then your own builds.
   When a package installs over a config file the backup already restored,
   apk keeps your file and puts its own next to it as `*.apk-new`. That is
   where the `.apk-new` files on the router come from.
4. **The PBX setup comes after the packages**, because it reads the restored
   `ari.conf` to decide whether to install ARI. It also keeps the restored
   SIP passwords.
5. **The firewall comes last.** It hardens SSH, and you need working SSH
   access to do everything before it.

## 2. The runbook

From the PC, in the repo folder. Use `/usr/bin/ssh`, not the local wrapper.

### Step 0 — before any reset (keep these current)

- A fresh **LuCI backup** (System → Backup / Flash Firmware → Generate
  archive). It contains the SIP passwords and TLS private keys, so store it
  like a password (doc 14 §5).
- The **package list**, re-taken whenever you add or remove a package:
  ```sh
  /usr/bin/ssh root@192.168.1.254 'sh -s -- snapshot' < scripts/router_packages.sh > scripts/router-packages.txt
  ```
- **Your own builds** in `fxs/build/`, matching the kernel the firmware will
  run: `kmod-slic-spi-cs1`, `kmod-mt7621-pcm-raw` (r3, 6.12.112) and
  `slic-fxs` (r6).

### Step 1 — after the reset

The router comes up as stock OpenWrt:
- LAN address **192.168.1.1**;
- root with **no password**;
- the stick not used.

Plug the PC into a LAN port. SSH will warn that the host key changed: run
`ssh-keygen -R 192.168.1.1` (and `-R 192.168.1.254` later).

### Step 2 — extroot

```sh
R=root@192.168.1.1
/usr/bin/ssh $R 'sh -s' < scripts/router_extroot.sh                        # status: "NOT ACTIVE"
/usr/bin/ssh $R 'sh -s -- attach --fresh' < scripts/router_extroot.sh      # dry run: read the plan
/usr/bin/ssh $R 'sh -s -- attach --fresh --apply' < scripts/router_extroot.sh
/usr/bin/ssh $R reboot
/usr/bin/ssh $R 'sh -s' < scripts/router_extroot.sh                        # expect "ACTIVE"
```

What `attach --fresh` does:
- finds the ext4 partition labelled `extroot`;
- **moves** the old overlay on it to `pre-attach-<time>/`. It is kept, not
  deleted: any file from before the reset can still be copied out of there;
- points fstab at the partition by UUID, and mounts the internal flash at
  `/rwm`;
- copies the router's current state onto the stick.

It never formats. Without an `extroot` partition it prints the `mkfs.ext4`
command and stops: formatting erases the stick, so that has to be your
decision.

`--reuse` instead boots the old overlay exactly as it was, packages
included. It is refused when the old overlay's kernel modules belong to a
different kernel, because then they can't load.

The partition is `LABEL=extroot`, ext4, 7.2 GiB (status output,
2026-11-17).

### Step 3 — the LuCI backup

Use LuCI at `http://192.168.1.1` (System → Backup / Flash Firmware → Restore
backup). The router restarts at **192.168.1.254**, with the WAN, your SSH
key and the root password.

The restore lands on the stick, because extroot is already active. Boot
still finds the stick through the internal flash's fstab, which step 2
wrote.

### Step 4 — feed packages

```sh
R=root@192.168.1.254
L=$(grep -v '^#' scripts/router-packages.txt | tr '\n' ' ')
/usr/bin/ssh $R "sh -s -- check $L"   < scripts/router_packages.sh   # what's missing
/usr/bin/ssh $R "sh -s -- restore $L" < scripts/router_packages.sh
```

`restore` refuses to run without extroot, or with less than 500 MiB free.
It runs `apk update` first, then installs everything missing in one
transaction. If one name has been renamed or dropped from the feed, apk
refuses the whole batch. The script then retries one package at a time and
names the ones that failed.

### Step 5 — your own builds

```sh
for f in fxs/build/kmod-slic-spi-cs1-*.apk fxs/build/kmod-mt7621-pcm-raw-*.apk fxs/build/slic-fxs-*-r6.apk; do
    /usr/bin/ssh $R "cat > /tmp/${f##*/}" < "$f"
done
/usr/bin/ssh $R 'apk add --allow-untrusted /tmp/kmod-slic-spi-cs1-*.apk /tmp/kmod-mt7621-pcm-raw-*.apk /tmp/slic-fxs-*.apk'
```

The kmods are tied to one kernel (6.12.112 here). After an OS upgrade with a
new kernel, rebuild them first (doc 09 §10).

### Step 6 — the PBX

```sh
/usr/bin/ssh $R slic-asterisk-setup
```

It does four things:
- **Installs** the PBX's modules, plus the ARI group (`res-ari` and all 12
  resources), because the restored `ari.conf` has ARI enabled.
- **Repairs** `/etc/asterisk` to mode 755, and creates the log and call-
  history folders.
- **Keeps** the restored SIP passwords.
- **Restarts** Asterisk and the phone daemon.

If the call agent isn't wanted, use `--no-ari`. The setup also prints any
Asterisk package installed by hand that it doesn't use.

Logs, call history and the time zone come back with the backup
(`asterisk.conf`, `/etc/config/system`); doc 13 §3.5 has the checks.

### Step 7 — firewall

```sh
/usr/bin/ssh $R 'sh -s' < scripts/router_fw_provision.sh             # dry run
/usr/bin/ssh $R 'sh -s -- --apply' < scripts/router_fw_provision.sh
/usr/bin/ssh $R 'sh -s' < scripts/router_fw_audit.sh                 # expect 0 FAIL
```

Open a **second** SSH session before closing the first: the provisioning
switches SSH to key-only (doc 14 §4.2).

### Step 8 — check that it works for you

1. Dial 600 (echo) and 601 (hello) from the handset.
2. Dial 100 from a softphone. Re-enter the passwords if the setup made new
   ones.
3. Start the call agent and dial `1717#`.
4. Reboot once. LuCI should show no unsaved changes, and the extroot status
   should say ACTIVE.

## 3. What was tested, and how

| Script | Tested how (2026-11-17) | Not tested |
|---|---|---|
| `router_extroot.sh` | `status` and the `attach` dry run live (correctly "already active"). `attach` on stand-in folders: no choice → refused; `--reuse` with another kernel's modules → refused; `--fresh --apply` → old overlay archived intact, current copied (symlinks included), fstab entries written; live fstab unchanged (md5). The tar copy keeps overlayfs whiteouts (`0,0` devices) on raw ext4. | the real mount and reboot after an actual reset |
| `router_packages.sh` | `snapshot` live (126 packages + 3 local); `check` live (0 missing, after fixing names provided by another package, e.g. `hostapd`); the `restore` failure path with a fake name (named it, `world` stayed clean) | a full `restore` onto an empty router |
| `slic-asterisk-setup` r6 | syntax; ARI group and leftover-package note: see doc 12 §8.1 | install from the r6 package |
| `router_fw_provision.sh` | doc 14 §4.3 | live `--apply` |

The stand-in run for `router_extroot.sh`, on the router:

```sh
T=/srv/extroot-test   # stand-ins: T/ovl = "/overlay", T/stick = the stick's root, T/conf = fstab copy
TEST_STICK=$T/stick TEST_OVERLAY=$T/ovl UCI_CONFDIR=$T/conf sh router_extroot.sh attach --fresh --apply
```

Trap: you can't create a whiteout (`mknod … c 0 0`) inside the overlay,
including `/srv`: overlayfs refuses it. Test whiteouts on the raw partition
under `/overlay/` instead.

## 4. The package list

`scripts/router-packages.txt` lists what was explicitly added on top of the
firmware image. That means `/etc/apk/world` minus the firmware's own
`/rom/etc/apk/world`: 126 packages on 2026-11-17, plus 3 locally built ones
as `# local:` comments.

Dependencies aren't listed, because apk resolves them again. To stop
restoring a package, delete its line. Some entries are heavy for a 248 MB
router: `gcc`, `git`, `clamav` (running), `airmon-ng`. Keeping them is a
choice, not a requirement.

**Removed on 2026-11-17** (unused, verified first): `asterisk-res-phoneprov`,
`asterisk-res-pjsip-phoneprov`, `asterisk-app-sms`,
`asterisk-format-g719/g723/g726/g729/h263/h264`, `asterisk-func-base64`,
`asterisk-func-talkdetect`, `asterisk-app-authenticate`,
`asterisk-res-clialiases`.

Phone provisioning (phoneprov) was running with nothing configured, and
hung an unused `/phoneprov` page on the web server nginx forwards. `app-sms`
is the landline SMS protocol, not the SMS gateway (smstools3). The formats
had no matching codec.

Afterwards Asterisk dropped from 73 to 60 packages (144 → 131 modules
loaded). ARI still answers `401` (loaded, waiting for a login), and a test
call to 601 played to the end. Before-state:
`/root/backups/apk-prune-20261117-121544/` (`world`, full installed list).

## 5. Open items

1. Run the whole sequence once for real: the next reset or a spare router.
2. Install `slic-fxs` r6 on the live router. It's only needed for the reset
   path, which installs it from `fxs/build` anyway.
3. Take a fresh LuCI backup now: the last one predates the prune and the
   firewall work.

## How obtained

- `/proc/mounts`, `block info`, `uci show fstab`, `/proc/mtd`, `df`, the
  ROM vs live package databases (`/rom/lib/apk/db/installed`,
  `/lib/apk/db/installed`, both `world` files).
- The 2026-11-14 boot log (`logread`: `block: attempting to load …`).
  Asterisk module change times (`ls -lc --full-time`) for when packages
  arrived: 2026-10-25 18:30–19:44 and 2026-11-05 00:12.
- Prune: `apk info -r` (reverse dependencies) before `apk del`; afterwards
  `module show`, `http show status`, `wget` on `/ari/asterisk/info`, and a
  call file to `Local/601@house`.
