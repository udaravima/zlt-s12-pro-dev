# 02 — Modem inventory

## What

| Property | Value |
|---|---|
| Identity | `sl8563-cpe-2h10` — UNISOC/Spreadtrum SL8563 LTE CPE |
| Arch | armv7l, kernel 4.4.83, proprietary firmware build 2025-07-09 |
| Serial (adb) | `<modem-serial>` |
| Storage | 128 MB NAND (124 MB `ubipac` UBI + boot chain) |
| Rootfs | ubifs, **read-only** (`ubi0:system on / type ubifs (ro)`) |
| Persistent writable | `/mnt/userdata` (a.k.a. `/mnt/data`, `/home`, `/var` — bind mounts of the same `userdata` UBI volume) |
| Free on userdata | 15.6 MB volume, 496 KB used, **14.6 MB free** |
| Init | full sysvinit (`/etc/init.d/*`) |
| adb | adbd running; USB functionfs + **adb over TCP on :5555** |
| ssh | **OpenSSH sshd RUNNING on :22** — see below |

## NAND / UBI layout (`cat /proc/mtd`, `/sys/class/ubi/ubi0/*`)

```
mtd0 splloader 256k   mtd4 ubootbak 1280k   mtd7 ubipac 124672k (UBI, mtd_num=7)
mtd1 uboot    1280k   mtd5 smlbak    768k
mtd2 sml       768k   mtd6 trustosbak 1M
mtd3 trustos    1M
```

16 UBI volumes inside `ubipac`: system 36.8M (ro root), **userdata 19.7M**,
prodnv 5.3M, recovery 14.7M, boot 9.5M, l_modem 14.7M, l_gdsp 6.3M, l_ldsp
4.3M, misc/pm_sys/fixnv/runtimenv/deltanv ~1.1M each. Sum ≈ 116M of 124M;
~8M unallocated (bad-block reserve). We will **not** touch the volume layout —
userdata's 14.6M free is plenty and safe.

## Userland (surprisingly rich)

BusyBox 1.27.2 plus a full userland: `sshd`, `scp`, `ssh`, `curl`, `tar`,
`losetup`, `python2.7`, `perl`, `sqlite3`, `strace`, `gdbserver`, `opkg` (no
feeds, useless), `nc`, `tftp` (client only). **No httpd/ftpd/tftpd, no
sftp-server binary** (`sshd_config` references `/usr/libexec/sftp-server`,
which does not exist — only `sshd_check_keys` is in `/usr/libexec/openssh/`).

## sshd details (already running, root-accessible)

From `/etc/ssh/sshd_config_readonly` (the config the running daemon uses):

```
Port 22 (listening 0.0.0.0 — netstat confirmed)
PermitRootLogin yes
PermitEmptyPasswords yes
AuthorizedKeysFile .ssh/authorized_keys
HostKey /var/run/ssh/ssh_host_*_key     (var = persistent userdata!)
```

Root's home is `/home/root` (per `/etc/passwd`) — **on persistent userdata**,
so an `authorized_keys` dropped there survives modem reboots. A router
dropbear ed25519 keypair (`/root/.ssh/id_dropbear` on the router) was generated
and its pubkey pushed to the modem at `/home/root/.ssh/authorized_keys`
(verified working — see 03).

## USB gadget — the hard constraint

- Gadget managed via configfs at `/sys/kernel/config/usb_gadget/g1`.
- Only function driver available: **`usb_f_rndis`** (`ls /sys/module | grep usb_f`).
  No `f_mass_storage`, no `f_ncm` module (NCM functions are built-in).
- Composition is firmware-controlled via `/mnt/userdata/data/usbenum.ini`:
  `virtualcn=7` → NCM4 (four NCM data channels) + diag/vendor serial ports +
  adb. The ini is persistent and could switch composition (e.g. to RNDIS),
  but **no mass-storage function exists to switch to**.
- Gadget-side network interfaces `usb0`–`usb3` exist on the modem but have no
  addresses (only `lo` configured).

**Consequence: the modem cannot present its storage as a USB block device to
the router. The only transports are adb (shell/push/pull/forward) and TCP over
the (currently unconfigured) NCM link.**

## Filesystems on the modem kernel

`/proc/filesystems`: vfat, msdos, exfat, ubifs + pseudo-fs. **No ext4, no
nfsd, no 9p** — so it can't serve NFS or host an ext4 image natively; it
would only store bytes (which is all we need).

## How obtained

From the router: `adb shell '...'` — `uname -a`, `df -h`, `mount`, `cat
/proc/mtd`, `cat /proc/partitions`, `cat /proc/cmdline`, `/sys/class/ubi/ubi0/*`
walk, `ls /sys/kernel/config/usb_gadget/`, `ls /sys/module | grep usb_f`, `cat
/mnt/userdata/data/usbenum.ini`, `ps w`, `netstat -tln`, `cat
/etc/ssh/sshd_config_readonly`, `grep root /etc/passwd`, `ls
/usr/libexec/openssh/`, `ip -o addr`, `ls /sys/class/net`, `cat
/etc/inittab`, `ls /etc/init.d`.
