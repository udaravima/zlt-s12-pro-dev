# 03 — Interconnect: how the router talks to the modem (verified working)

> **Update 2026-09-26 (later same day):** a direct NCM management link now
> exists (usb2, 192.168.9.1/30 ↔ 192.168.9.2/30) with raw throughput of
> 5.9–18.8 MB/s — see `05-expansion.md`. The adb-forward ssh path below
> remains the reliable *interactive* path; **do not use scp for bulk in either
> path** (modem OOM-kills sshd on large receives — diagnosed in 05).

## The access chain (all verified live)

```
host PC ──/usr/bin/ssh──> router
                            │
                            ├── adb shell  ─────> modem root shell
                            ├── adb push/pull ──> modem filesystem (incl. persistent /mnt/userdata)
                            └── adb forward tcp:2222 -> modem tcp/22
                                    └── ssh -p 2222 -i /root/.ssh/id_dropbear root@127.0.0.1
                                        (key auth, verified: "SSH_OK")
```

Facts that make this chain work:

1. Router runs an adb fork-server at boot (`/etc/rc.local` →
   `/etc/init.d/adb-enablemodem`), modem adbd answers over USB functionfs.
2. Router dropbear keypair generated at `/root/.ssh/id_dropbear` (ed25519,
   fingerprint SHA256:7I9QmqScMvqNZgCPs7ss0anOc8qkCLzXF5NufGSyX60).
3. Pubkey installed on modem at `/home/root/.ssh/authorized_keys` — on the
   persistent userdata volume, so it survives modem reboots. Permissions
   700/600 (required; sshd would reject otherwise).
4. `adb forward` maps router localhost:2222 → modem:22. No network
   configuration on either side is needed for this path.

Gotcha learned: piping stdin into `adb shell 'cat > file'` silently produced
a 0-byte file — **use `adb push` for file transfer**, not shell redirection.

## Benchmarks (5 MB random file, md5-verified round-trip)

| Method | Throughput | Notes |
|---|---|---|
| `adb push` (router→modem) | **4.0 MB/s** | 5MB in 1.26 s |
| `adb pull` (modem→router) | **1.1 MB/s** | 5MB in 4.47 s |
| `scp` via adb forward | **~1.9 MB/s** | 5MB in 2.79 s |

All are ample for package trees (a 20 MB `/opt` tree restores in ~5–20 s at
boot). Test files were cleaned up on both sides afterwards.

## Alternative transport (not yet configured)

The gadget exposes 4 NCM pairs; router `usb0`–`usb3` and modem `usb0`–`usb3`
exist but are DOWN/unaddressed. Giving both ends static IPs (e.g. router
usb0 = 192.168.9.1/30, modem usb0 = 192.168.9.2/30 via `adb shell` at router
boot) would give direct IP networking — useful if we later want a live
network filesystem, or want to drop the adb-forward indirection. The modem's
USB composition can also be switched via `usbenum.ini` (`virtualcn=1` →
RNDIS) if NCM gives trouble on the router side — but that changes the
WAN-side behavior too (NCM channels are used for data), so it was NOT touched.

## What is persistent where

| Data | Lives on | Survives |
|---|---|---|
| `authorized_keys` (modem) | `/home/root/.ssh` = userdata ubifs | modem reboot ✓ |
| sshd host keys (modem) | `/var/run/ssh` = userdata bind | modem reboot ✓ |
| Future package store | `/mnt/userdata/...` | modem reboot ✓ |
| Router dropbear keypair | `/root/.ssh` in overlay | router reboot ✓ |
| `adb forward` rules | not persistent | must be re-run at boot (script) |

## How obtained

Executed on the router: `adb push/pull` with a 5 MB `/dev/urandom` file and
`md5sum` comparison; `time adb push|pull`, `time scp`; `adb forward tcp:2222
tcp:22`; `ssh -y -i /root/.ssh/id_dropbear -p 2222 root@127.0.0.1 'echo
SSH_OK'`.
