#!/bin/sh
# Runs ON THE ROUTER (OpenWrt 25.x, BusyBox ash). Step 1 after a factory reset.
#
# Extroot = the USB stick's ext4 partition used as the overlay, the writable
# layer over the read-only firmware. The router itself has only 4.1 MiB of
# writable flash (rootfs_data, jffs2) next to a 12.5 MiB read-only, full
# firmware (rootfs, squashfs): every package and the PBX need the stick first.
#
#   /usr/bin/ssh root@<router> 'sh -s' < scripts/router_extroot.sh                       # status (read-only)
#   /usr/bin/ssh root@<router> 'sh -s -- attach' < scripts/router_extroot.sh             # dry run
#   /usr/bin/ssh root@<router> 'sh -s -- attach --fresh --apply' < scripts/router_extroot.sh
#   ... then: reboot, and run status again: it should say ACTIVE.
#
# attach (only when / is NOT already on the stick):
#   - finds the ext4 partition labelled "extroot" (or DEV=/dev/sdXN); never
#     formats. No such partition: prints the mkfs command and stops.
#   - an existing overlay on it (upper/, work/) needs a decision:
#       --fresh  move it aside to pre-attach-<time>/ on the stick (kept, not
#                deleted: old configs and files stay readable there) and start
#                from the router's current state. The normal choice after a
#                reset or an OS upgrade.
#       --reuse  boot the old overlay as it is: the state from before the
#                reset comes back wholesale, packages included. Only sane on
#                the SAME firmware build (the script checks the kernel version).
#   - points fstab at the partition (target /overlay, by UUID) and mounts the
#     internal flash at /rwm, then copies the current overlay (including that
#     fstab) onto the stick.
# The firmware already contains block-mount, kmod-usb-storage, kmod-fs-ext4
# and e2fsprogs here, so attach installs nothing.
#
# Test against stand-ins (no mount, no active check; see doc 15 §3):
#   TEST_STICK=<dir as the stick's root> TEST_OVERLAY=<dir as /overlay> UCI_CONFDIR=<fstab copy dir>

mode=${1:-status}
[ $# -gt 0 ] && shift
APPLY=0
CHOICE=""
for a; do
	case $a in
	--apply) APPLY=1 ;;
	--fresh) CHOICE=fresh ;;
	--reuse) CHOICE=reuse ;;
	*) echo "unknown option: $a" >&2; exit 2 ;;
	esac
done
if [ -n "${UCI_CONFDIR:-}" ]; then
	mkdir -p "$UCI_CONFDIR/.save"
	U="uci -c $UCI_CONFDIR -t $UCI_CONFDIR/.save"
else
	U=uci
fi
OVL=${TEST_OVERLAY:-/overlay}

die() { echo "router_extroot: $*" >&2; exit 1; }
say() { if [ $APPLY = 1 ]; then echo "  do    $*"; else echo "  plan  $*"; fi; }
run() { if [ $APPLY = 1 ]; then "$@"; fi; }

# "dev type" of what is mounted on /overlay (last mount wins)
overlay_mount() { awk '$2 == "/overlay" { d = $1; t = $3 } END { print d, t }' /proc/mounts; }
is_active() {
	set -- $(overlay_mount)
	case $1:$2 in /dev/sd*:ext4 | /dev/sd*:f2fs | /dev/mmcblk*:ext4 | /dev/mmcblk*:f2fs) return 0 ;; esac
	return 1
}
# block info field $2 of device $1
binfo() { block info 2>/dev/null | grep "^$1:" | sed -n "s/.* $2=\"\\([^\"]*\\)\".*/\\1/p"; }
find_dev() {
	[ -n "${DEV:-}" ] && { echo "$DEV"; return; }
	d=$(block info 2>/dev/null | grep 'LABEL="extroot"' | grep 'TYPE="ext4"' | cut -d: -f1 | head -1)
	[ -n "$d" ] && { echo "$d"; return; }
	# else: the only ext4 partition on a USB/SD disk
	d=$(block info 2>/dev/null | grep -E '^/dev/(sd|mmcblk)' | grep 'TYPE="ext4"' | cut -d: -f1)
	[ "$(echo "$d" | grep -c .)" = 1 ] && echo "$d"
}
mib() { df -k "$1" 2>/dev/null | awk 'NR == 2 { printf "%d MiB used, %d MiB free", $3 / 1024, $4 / 1024 }'; }

status() {
	set -- $(overlay_mount)
	if is_active; then
		echo "ACTIVE: / runs on $1 ($2), $(mib /overlay)"
	else
		echo "NOT ACTIVE: /overlay is $1 ($2), $(df -k /overlay | awk 'NR == 2 { printf "%d KiB free", $4 }')"
		echo "  -> run: attach --fresh --apply, then reboot"
	fi
	d=$(find_dev)
	if [ -n "$d" ]; then
		echo "stick: $d  label=$(binfo "$d" LABEL)  uuid=$(binfo "$d" UUID)  $(($(cat /sys/class/block/${d#/dev/}/size 2>/dev/null || echo 0) / 2048)) MiB"
	else
		echo "stick: no ext4 partition labelled 'extroot' (or not exactly one ext4 partition) found"
	fi
	echo "fstab mounts:"
	$U -X show fstab 2>/dev/null | sed -n "s/^fstab\.\([^.=]*\)=mount\$/\1/p" | while read -r s; do
		echo "  $s: $($U -q get "fstab.$s.uuid")$($U -q get "fstab.$s.device") -> $($U -q get "fstab.$s.target") (enabled=$($U -q get "fstab.$s.enabled" || echo 1))"
	done
	if is_active; then
		ls -d /overlay/pre-attach-* 2>/dev/null | sed 's/^/archived overlay: /'
		[ -d /rwm/upper ] && echo "internal flash at /rwm: $(du -sk /rwm/upper | cut -f1) KiB in use"
	fi
}

attach() {
	echo "attach$([ $APPLY = 1 ] || echo ' (dry run: nothing is written; add --apply)')"
	if [ -z "${TEST_STICK:-}" ] && is_active; then
		echo "extroot is already active; nothing to do"
		exit 0
	fi
	if [ -n "${TEST_STICK:-}" ]; then
		STICK=$TEST_STICK
		uuid=TEST-UUID
	else
		d=$(find_dev)
		[ -n "$d" ] || die "no ext4 extroot partition found. To create one (ERASES the stick):
    mkfs.ext4 -L extroot /dev/sda1      (then run attach again; or set DEV=/dev/sdXN)"
		uuid=$(binfo "$d" UUID)
		[ -n "$uuid" ] || die "$d has no UUID"
		# auto_mount may already have mounted it (e.g. at /mnt/sda1)
		STICK=$(awk -v d="$d" '$1 == d { print $2; exit }' /proc/mounts)
		if [ -z "$STICK" ]; then
			STICK=/tmp/extroot-attach
			echo "  mount $d at $STICK (read-write, to inspect and prepare)"
			mkdir -p "$STICK" && mount -t ext4 "$d" "$STICK" || die "cannot mount $d"
			trap 'umount "$STICK" 2>/dev/null' EXIT
		fi
		echo "  stick: $d uuid $uuid, mounted at $STICK"
	fi

	if [ -d "$STICK/upper" ]; then
		echo "  the stick already holds an overlay: $(du -sk "$STICK/upper" | cut -f1) KiB, from $(date -r "$STICK/upper" '+%Y-%m-%d')"
		case $CHOICE in
		fresh)
			A=$STICK/pre-attach-$(date +%Y%m%d-%H%M%S)
			say "move the old overlay aside to ${A#$STICK/}/ (kept, not deleted)"
			run mkdir "$A" && run mv "$STICK/upper" "$A/" && { [ -d "$STICK/work" ] && run mv "$STICK/work" "$A/" || :; } ||
				{ [ $APPLY = 1 ] && die "could not move the old overlay aside"; } ;;
		reuse)
			old=$(ls "$STICK/upper/lib/modules" 2>/dev/null | head -1)
			if [ -n "$old" ] && [ "$old" != "$(uname -r)" ]; then
				die "--reuse refused: the old overlay has kernel modules for $old, this firmware runs $(uname -r). Use --fresh."
			fi
			say "keep the old overlay as the new root (state from before the reset returns)" ;;
		*) die "choose --fresh (archive it, start from now) or --reuse (boot it as it was); see the header" ;;
		esac
	else
		CHOICE=fresh
	fi

	# fstab first, so the copy below carries it onto the stick too
	for s in $($U -X show fstab 2>/dev/null | sed -n "s/^fstab\.\([^.=]*\)=mount\$/\1/p"); do
		[ "$s" = extroot ] && continue
		[ "$($U -q get "fstab.$s.target")" = /overlay ] && { say "drop old fstab mount '$s' (also targets /overlay)"; run $U delete "fstab.$s"; }
	done
	say "fstab: extroot = uuid $uuid -> /overlay"
	run $U set fstab.extroot=mount
	run $U set fstab.extroot.uuid="$uuid"
	run $U set fstab.extroot.target=/overlay
	run $U set fstab.extroot.enabled=1
	rd=$(sed -n 's/^mtd\([0-9]*\):.*"rootfs_data"$/\1/p' /proc/mtd)
	if [ -n "$rd" ]; then
		say "fstab: rwm = /dev/mtdblock$rd -> /rwm (the internal flash stays reachable)"
		run $U set fstab.rwm=mount
		run $U set fstab.rwm.device="/dev/mtdblock$rd"
		run $U set fstab.rwm.target=/rwm
	fi
	run $U commit fstab

	if [ "$CHOICE" = fresh ]; then
		say "copy the current overlay ($(du -sk "$OVL/upper" 2>/dev/null | cut -f1) KiB) to the stick"
		if [ $APPLY = 1 ]; then
			mkdir -p "$STICK/upper" "$STICK/work" || die "cannot create upper/work on the stick"
			tar -C "$OVL/upper" -cf - . | tar -C "$STICK/upper" -xf - || die "copy failed"
		fi
	else
		say "copy the new fstab into the old overlay"
		run mkdir -p "$STICK/upper/etc/config"
		run cp -p "${UCI_CONFDIR:-/etc/config}/fstab" "$STICK/upper/etc/config/fstab"
	fi
	[ $APPLY = 1 ] && sync
	echo
	if [ $APPLY = 1 ]; then
		echo "done. Reboot now (reboot), then run this script again: it should say ACTIVE."
	else
		echo "re-run with --apply to do it"
	fi
}

case $mode in
status) status ;;
attach) attach ;;
*) echo "usage: sh -s [-- status | attach [--fresh|--reuse] [--apply]]" >&2; exit 2 ;;
esac
