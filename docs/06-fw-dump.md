# 06 — Modem firmware dump (full NAND image)

Performed 2026-09-26. Result: **all 8 MTD partitions dumped, 128 MB total,
stored at `modem-fw-dump/`** on the host. Verified by md5 against
modem-side hashes and by a repeat-dump byte comparison.

## Dumped files

| File | Partition | Size | md5 (verified) |
|---|---|---|---|
| `mtd0-splloader.bin` | splloader | 256 KB | `00269b48d22c6c982c4d96db4f405e42` |
| `mtd1.bin` | uboot | 1.25 MB | `de32673d75c690c3b8d995170006a6d6` |
| `mtd2.bin` | sml | 768 KB | `eed6129fd657aa0e3c86dd9b5cc9f5c4` |
| `mtd3.bin` | trustos | 1 MB | `2165d94df27ad13c8040cea61b5cbf8a` |
| `mtd4.bin` | ubootbak | 1.25 MB | `de32673d…` (identical to mtd1) |
| `mtd5.bin` | smlbak | 768 KB | `eed6129f…` (identical to mtd2) |
| `mtd6.bin` | trustosbak | 1 MB | `2165d94d…` (identical to mtd3) |
| `mtd7-ubipac.bin` | ubipac (all 16 UBI volumes incl. userdata) | 122 MB | see note |

Reference modem-side hashes in `reference-md5s.txt`.

## Integrity notes (the interesting part)

- mtd0–6: md5-verified against hashes computed **on the modem** — exact.
- Backup partitions (mtd4/5/6) are byte-identical to their primaries — nice
  confirmation the dump is faithful (two independent reads agreeing).
- mtd7 first appeared to "mismatch": reference `f6536bdd…` (pre-dump) vs
  `ca91a880…` (dump + fresh modem hash). Diagnosis: **mtd7 is a live UBI
  device** — `/var` is persistent userdata inside it and the modem writes
  logs there continuously, so any whole-partition hash is a snapshot in time.
  Proof of dump fidelity: a **second full 3-minute dump was byte-identical**
  (`cmp -l` = 0 differences, md5 `ca91a88094e9beb3939b17309b685c6f` for both).

## How it was done (and why)

Transport chain, zero bytes stored on router or modem:

```
modem: python /mnt/userdata/tools/modem_send.py /dev/mtdblock<N> 5530
   → NCM link (usb2, 192.168.9.2) at ~7 MB/s sustained
router: nc 192.168.9.2 5530   (via scripts/router_dump_mtd.sh, streamed)
   → host: ssh pipe straight into modem-fw-dump/mtdN.bin
```

Pitfalls solved (documented so nobody rediscovers them):

1. **No `adb exec-out`** — router's adb is from 2014 (`adb exec-out true` →
   "error: closed"), and plain `adb shell` mangles binary via pty CRLF.
   Hence the python sender over NCM.
2. **Modem-side children die with their adb shell** — the 2014 adb server
   kills spawned processes when the launching shell session exits. Fix:
   `router_dump_mtd.sh` holds the spawn adb shell open in the background for
   the whole transfer, polls `netstat` until the listener is up, then execs
   `nc`. (Same reason earlier `nohup … &` attempts vanished silently.)
3. **Avoided ssh/scp entirely for bulk** — the modem OOM-kills sshd on big
   receives (see 05). adb push (4 MB/s) was too slow for 122 MB; NCM raw
   stream does it in ~3 minutes.

Tools (persistent, reusable):
- `scripts/modem_send.py` — modem-side streamer (also installed at
  modem `/mnt/userdata/tools/modem_send.py`).
- `scripts/router_dump_mtd.sh` — router-side orchestrator; usage:
  `ssh root@router.lan "MTD=N sh -s" < router_dump_mtd.sh > mtdN.bin`

## What this unlocks

- **Full brick insurance**: with splloader/uboot/sml/trustos + ubipac images
  we have everything needed to restore the modem's NAND (via UNISOC
  flash tools / u-boot repair paths) if anything ever corrupts it.
- Offline analysis: the ubipac image contains the 16 UBI volumes (system
  36.8M ro rootfs, recovery, l_modem, DSP firmwares) — can be unpacked with
  `ubireader_extract_image` on the host to study the modem's firmware without
  touching the device.
- Note: dump = read-only operation; nothing on the modem was modified.
