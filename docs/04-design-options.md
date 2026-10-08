# 04 — Design options: modem storage as router package storage

> **Update 2026-09-26 (later same day):** the recommended design below still
> stands, with one improvement from `05-expansion.md`: restore the /opt
> tarball via the direct NCM link (raw stream 5.9–18.8 MB/s, or a tiny httpd
> on the modem) instead of `adb pull` (1.1 MB/s) — a 20 MB tree then restores
> in ~1–4 s. The "plan B" (sshfs live mount) is now judged **worse than
> stated**: the modem's 39 MB RAM makes any encrypted-buffered live filesystem
> fragile (OOM-killed sshd, see 05); prefer static binaries + sync model.
> Brick risk is also retired as a concern: full NAND dump exists (`06-fw-dump.md`).

Goal: a few MB of the modem's 14.6 MB free userdata, usable as persistent
package storage for the router.

## Dead ends (checked, with reasons)

| Idea | Why it fails |
|---|---|
| USB mass-storage gadget | Modem kernel has **no `f_mass_storage`** function driver (only `usb_f_rndis`). Nothing to export a block device with. |
| NFS / 9p / CIFS mount | Modem kernel/userland has **no server** for any of these (`/proc/filesystems`, binary inventory checked). |
| sshfs live mount | (a) modem's sshd has **no sftp-server** binary; (b) router has no FUSE (`kmod-fuse` would eat overlay); (c) `sshfs` is not in the OpenWrt 25.12 package feed — would need a custom mipsel build. Possible, but the most moving parts. |
| ext4 image + loop mount | The image file lives on the modem; with no network block device it must still be **copied to RAM** before mounting — so the kmods (`kmod-fs-ext4`+`kmod-loop` ≈ 200–300 KB of the last 356 KB of overlay) buy nothing over a plain tarball. Strictly worse than the recommended design. |
| New UBI volume on the modem | Works, but unnecessary — userdata already has 14.6 MB free; repartitioning adds risk for zero benefit. |

## Recommended: "Entware-style /opt in RAM + modem as the disk"

The router has 120 MB of free tmpfs in `/tmp`. Install extra packages into an
`/opt` tree that lives in RAM and is **persisted as a tarball on the modem**.

```
boot:  adb pull /mnt/userdata/router/opt.tar.gz → /tmp
       tar -xzf → /tmp/opt ;  /opt → /tmp/opt (symlink, in overlay)
use:   PATH=/opt/usr/bin:$PATH, LD_LIBRARY_PATH=/opt/usr/lib (profile.d)
       apk --root /tmp/opt add <pkg>      (apk 3.0.5 supports --root/-p)
sync:  tar -czf /tmp/opt.tar.gz -C /tmp opt && adb push → modem
       (after every apk operation, on shutdown, and via cron)
```

Why this wins:

- **Zero kmods, zero new binaries on the router** — the whole router-side
  footprint is one init script + one profile script (~2 KB of overlay). All
  transports (adb pull/push, ssh) are already proven working today.
- apk-tools 3.0.5 on the router supports `--root ROOT` (verified in `apk add
  --help`) and `--initdb`/`--usermode`, so `/tmp/opt` is a real apk-managed
  system with dependency resolution — not manual file copying.
- RAM cost 5–20 MB (of 120 MB free); boot cost ~5–20 s for a 20 MB tree at
  the measured 1.1–1.9 MB/s.
- The modem's copy is a plain file on persistent ubifs; keep the previous
  tarball as `opt.tar.gz.1` so a bad sync never loses the last good state.

Honest limitations of this design:

- **Kernel modules (kmod-*) cannot go in /opt** — they must sit in the real
  `/lib/modules/<kver>` and be installed into the real root (overlay). This
  design extends userspace only.
- **Power-loss window**: packages added between the last sync and a power cut
  are lost (the sync'd tarball survives; re-run `apk add`). Mitigate with
  sync-on-change + a daily cron. Router reboots are rare (this box: soft
  reboots, USB power stays up, modem doesn't even reboot).
- `/opt`-installed daemons need init handling in the boot script (start after
  extraction), and LuCI apps won't integrate cleanly (uhttpd config lives in
  the real root) — pick CLI-ish packages for /opt.

## Complementary actions (independent of the above)

1. **Free the overlay first** — `luci-theme-bootstrap` static assets alone are
   **1.5 MB = 45% of the overlay partition**. Removing that theme (or moving
   its static dir into the /opt scheme) may be the single highest-ROI action.
   Also delete `/etc/config/*.bak-*` and resolve `.apk-new` leftovers.
2. **Modem as apk cache**: keep fetched `.apk` files on the modem and point
   apk at them (`--cache-dir /tmp/apkcache` populated by adb pull at boot) so
   packages never re-download from the internet.
3. **Long-term proper fix**: a custom firmware build with a slimmer package
   set for 16 MB flash, or hardware with more flash.

## Alternative if live-persistence is ever needed

Configure the NCM link with static IPs (both ends have usb0 ready), push an
armv7 `sftp-server` binary to modem userdata (executable there — mount is
`rw,nosuid,nodev`, no `noexec`), build sshfs for mipsel, and live-mount.
Real-time writes, no sync dance — at the cost of a custom build, `kmod-fuse`
in overlay, and a boot dependency on the modem. Keep as plan B.

## Open questions to decide before implementing

1. Which packages should live in /opt (candidates: the extras — collectd?,
   mwan3 helpers, usteer, upnpd — or new things like tmux/htop/rsync)?
2. Sync policy: on-change hook vs cron-only vs both?
3. Do we also do the theme cleanup in the same pass?
4. Tarball on modem userdata vs. the ~8 MB unallocated UBI space (recommend:
   userdata, simpler).
