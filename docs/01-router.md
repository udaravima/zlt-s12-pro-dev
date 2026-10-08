# 01 — Host router inventory (ZLT S12 Pro)

## What

| Property | Value |
|---|---|
| Model | TOZED ZLT S12 PRO (`tozed,zlt-s12-pro`) |
| SoC | MediaTek MT7621 ver:1 eco:3 (mipsel) |
| Firmware | OpenWrt **25.12.5** r33051-f5dae5ece4, target `ramips/mt7621`, arch `mipsel_24kc` |
| Kernel | 6.12.94 (SMP) |
| Rootfs type | squashfs + overlayfs (read-only rom + jffs2 overlay) |
| Package manager | **apk-tools 3.0.5** — there is NO opkg on this box |
| RAM | 248 MB; `/tmp` tmpfs = 121 MB with ~120 MB free |
| Flash | 16 MB NOR/NAND, 100% allocated (see below) |
| WAN | `pppoe-slt_inter` (default route via 100.64.1.1), VLANs wan.10/wan.20 on eth0 |
| LAN | `lan_br` 192.168.1.254/24 |

## Flash layout (`cat /proc/mtd`)

```
mtd0 00030000 u-boot        (192KB)
mtd1 00008000 u-boot-env    (32KB)
mtd2 00008000 tozed-conf    (32KB)
mtd3 00010000 factory       (64KB)
mtd4 00fb0000 firmware      (16.1MB total container)
mtd5 0032ae15 kernel        (3.2MB)
mtd6 00c851eb rootfs        (12.5MB, squashfs rom 9.0MB + rootfs_data)
mtd7 003a0000 rootfs_data   (3.6MB jffs2 → mounted /overlay)
```

## Overlay pressure — the actual problem

`df -h`: `/dev/mtdblock7` on `/overlay`: **3.6M size, 3.3M used, 356K free (90%)**.

Uncompressed content in `/overlay/upper` (`du -s`, jffs2 compresses on flash,
so du > df):

| Path | Size (uncompressed) | Notes |
|---|---|---|
| `usr/lib` | 2.4 MB | package libraries |
| `usr/sbin` | 1.9 MB | package binaries |
| `www/luci-static/resources` | **1.5 MB** | **luci-theme-bootstrap static assets — 45% of the whole partition** |
| `usr/bin` | 0.45 MB | |
| `lib` | 0.47 MB | |
| `etc` | 0.1 MB | incl. several `.bak-*` and `.apk-new` config leftovers |

287 packages installed. Notable space consumers (extras over base): luci
bootstrap theme, collectd + luci_statistics, mwan3, usteer, upnpd, ddns,
https-dns-proxy, wifi_schedule.

**Cleanup candidates** (biggest single win first):
1. `luci-theme-bootstrap` static resources = 1.5 MB → remove/switch theme.
2. `/etc/config/*.bak-*` files (firewall x2, network, https-dns-proxy).
3. Question each heavy package (collectd/luci-statistics etc.).

## Kernel/filesystem capabilities (matters for the design)

`cat /proc/filesystems`: only `squashfs jffs2 ubifs overlay` + pseudo-fs.
**No ext4, no fuse, no nfs, no 9p, no cifs** — any of those would need kmods
installed into the nearly-full overlay.

Loaded USB/net kmods: `cdc_ether`, `cdc_ncm`, `huawei_cdc_ncm`, `rndis_host`,
`option`, `usb_wwan`, `usbserial`, `xhci_*` — the modem's USB drivers are all
present.

Host-side NCM network interfaces `usb0`–`usb3` exist but are DOWN and
unconfigured (modem gadget enumerates 4 CDC-EEM/ECM pairs + vendor/diag ports
+ adb on functionfs).

## Boot behavior relevant to the design

- `/etc/rc.local` runs `/etc/init.d/adb-enablemodem` → **adb fork-server
  starts at every boot** (verified: init script exists, server was running).
- Router soft-rebooted during our session (uptime 16 min) while the modem
  stayed up 2 days → adb server restart is already handled.

## How obtained

`/usr/bin/ssh root@router.lan` with: `ubus call system board`,
`cat /etc/openwrt_release`, `df -h`, `mount`, `cat /proc/mtd`, `du -s
/overlay/upper/...`, `apk --version`, `apk info | wc -l`, `cat
/proc/filesystems`, `lsmod`, `ip -br addr`, `ls /etc/init.d`, `cat /etc/rc.local`.
