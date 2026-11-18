#!/bin/sh
# Runs ON THE ROUTER (OpenWrt 25.x, BusyBox ash).
#
# The LTE modem's USB serial ports: what each one is, who is meant to use it,
# and who actually does (doc 17).
#
#   status   the map, who holds each port, and any clash (read-only; default)
#   apply    put the LTE dial (wwan) on ttyUSB3 and smsd on ttyUSB4, and stop
#            the modem's registration reports reaching smsd; backs up first
#
#   /usr/bin/ssh root@<router> sh -s < scripts/router_modem_ports.sh
#   /usr/bin/ssh root@<router> 'sh -s -- apply' < scripts/router_modem_ports.sh
#
# The map, matched from both ends (the router's USB interfaces, and on the
# modem its USB gadget config, /etc/atrouter/system.ini, /etc/engpc/dev/pc.conf):
#
#   router   USB if  modem side                         meant for
#   ttyUSB0  08      ttyGS2, atrouter AT channel 0      people and tools (sms_tool, manual AT)
#   ttyUSB1  09      ttyGS0, engpc: modem diagnostics   nobody (binary)
#   ttyUSB2  0a      ttyGS1, engpc: modem log stream    nobody (binary)
#   ttyUSB3  0b      ttyGS3, atrouter AT channel 1      the LTE dial (network.wwan.device)
#   ttyUSB4  0d      ttyGS4, atrouter AT channel 2      smsd (/etc/smsd.conf)
#   (if 0c is adb; if 00-07 are four NCM network links, usb0-usb3)
#
# Why this split:
# - ModemManager, when it enables the modem, takes ttyUSB0 (primary) and
#   ttyUSB4 (secondary). ttyUSB3 is the AT port it leaves alone, so the dial
#   goes there; doc 08 (2026-10-02) dialled there with ModemManager running.
# - smsd keeps its port open all the time, so it needs one nobody dials on.
#   After a factory reset the stock board script puts wwan on ttyUSB0, so
#   smsd stays off ttyUSB0 too. That leaves ttyUSB4. Keep ModemManager
#   disabled (it is today), or it would share ttyUSB4 with smsd.
# - Registration reports (+CREG/+CEREG mode 2) are on for every AT channel.
#   smsd logged each one as "Unexpected input": 20 % of the system log.
#   apply adds "AT+CREG=0;+CEREG=0" to smsd's init string.
#
# ttyUSB numbers come from the order the option driver binds the interfaces.
# They stay put unless another USB serial device appears first; status checks
# each port's interface number and warns if the numbering has moved.

VID=1782 PID=4055
WANT_WWAN=/dev/ttyUSB3 WANT_SMSD=/dev/ttyUSB4
SMSD_INIT='AT+CREG=0;+CEREG=0'
SMSD_CONF=/etc/smsd.conf
problems=0

say() { printf '%s\n' "$*"; }
warn() { say "WARN: $*"; problems=$((problems + 1)); }
die() { say "ERROR: $*" >&2; exit 1; }

what() {	# USB interface number -> what it is | what it's for
	case $1 in
	08) echo "AT channel 0 (modem ttyGS2)|people and tools" ;;
	09) echo "modem diagnostics, binary (engpc)|nobody" ;;
	0a) echo "modem log stream, binary (engpc)|nobody" ;;
	0b) echo "AT channel 1 (modem ttyGS3)|LTE dial (wwan)" ;;
	0d) echo "AT channel 2 (modem ttyGS4)|smsd" ;;
	*) echo "unknown|?" ;;
	esac
}
expected_if() { case $1 in ttyUSB0) echo 08 ;; ttyUSB1) echo 09 ;; ttyUSB2) echo 0a ;; ttyUSB3) echo 0b ;; ttyUSB4) echo 0d ;; esac; }

modem_present() {
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = "$VID:$PID" ] && return 0
	done
	return 1
}

ifnum() {	# ttyUSBn -> USB interface number of the modem, or "-" if not the modem
	local dev
	dev=$(readlink -f "/sys/class/tty/$1/device/..") || { echo -; return; }
	[ "$(cat "$dev/../idVendor" 2>/dev/null):$(cat "$dev/../idProduct" 2>/dev/null)" = "$VID:$PID" ] || { echo -; return; }
	cat "$dev/bInterfaceNumber" 2>/dev/null || echo -
}

smsd_device() { sed -n 's/^[[:space:]]*device[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p' "$SMSD_CONF" 2>/dev/null | head -n 1; }
smsd_init() { sed -n 's/^[[:space:]]*init[[:space:]]*=[[:space:]]*\(.*\)$/\1/p' "$SMSD_CONF" 2>/dev/null | head -n 1; }

configured_for() {	# /dev/ttyUSBn -> who is configured to use it
	local out="" s
	for s in $(uci -q show network | sed -n "s/^network\.\([^.]*\)\.device='\(.*\)'$/\1 \2/p" | while read -r i d; do
			[ "$(readlink -f "$d" 2>/dev/null)" = "$1" ] && echo "network.$i"
		done); do out="$out $s"; done
	[ "$(readlink -f "$(smsd_device)" 2>/dev/null)" = "$1" ] && out="$out smsd"
	echo "${out# }"
}

OPEN=""
scan_open() {	# one pass over every process's open files: "pid /dev/ttyUSBn" lines
	# (a readlink per file would fork thousands of times; ls -l does it in one)
	OPEN=$(ls -l /proc/[0-9]*/fd 2>/dev/null | awk '
		/^\/proc\/[0-9]+\/fd:$/ { split($0, a, "/"); pid = a[3]; next }
		/-> \/dev\/ttyUSB[0-9]+$/ { print pid, $NF }' | sort -u)
}
holders() {	# /dev/ttyUSBn -> processes with it open (from scan_open)
	local out="" pid dev
	while read -r pid dev; do
		[ "$dev" = "$1" ] && out="$out $(cat "/proc/$pid/comm" 2>/dev/null)"
	done <<EOF
$OPEN
EOF
	echo "${out# }"
}

status() {
	modem_present || die "modem $VID:$PID not on USB"
	scan_open
	printf '%-8s %-3s %-38s %-17s %-18s %s\n' port if "is" "meant for" "configured for" "open by"
	for t in /sys/class/tty/ttyUSB*; do
		[ -e "$t" ] || continue
		n=${t##*/}; i=$(ifnum "$n"); w=$(what "$i")
		c=$(configured_for "/dev/$n"); h=$(holders "/dev/$n")
		printf '%-8s %-3s %-38s %-17s %-18s %s\n' "$n" "$i" "${w%%|*}" "${w##*|}" "${c:--}" "${h:--}"
		e=$(expected_if "$n")
		[ "$i" = - ] || [ -z "$e" ] || [ "$i" = "$e" ] || warn "$n is USB interface $i, expected $e: the numbering has moved, fix the map before trusting the names"
		case "$c" in *" "*) warn "$n is configured for more than one user: $c" ;; esac
		case "$i" in 09|0a) [ -n "$c" ] && warn "$n is a binary modem stream but is configured for: $c" ;; esac
	done
	wd=$(readlink -f "$(uci -q get network.wwan.device)" 2>/dev/null)
	sd=$(readlink -f "$(smsd_device)" 2>/dev/null)
	[ "$wd" = "$WANT_WWAN" ] || say "NOTE: wwan dials on ${wd:-?}; the plan is $WANT_WWAN (apply moves it)"
	[ "$sd" = "$WANT_SMSD" ] || say "NOTE: smsd uses ${sd:-?}; the plan is $WANT_SMSD (apply moves it)"
	[ "$(smsd_init)" = "$SMSD_INIT" ] || say "NOTE: smsd's init string is '$(smsd_init)'; apply sets '$SMSD_INIT' to stop registration reports"
	mm=$(mmcli --timeout=5 -m 0 -K 2>/dev/null | sed -n 's/^modem\.generic\.state *: *//p')
	case "$mm" in
	"" | disabled | failed) say "ModemManager modem state: ${mm:-none}" ;;
	*) warn "ModemManager modem state is '$mm': it holds ttyUSB0 and ttyUSB4 and will clash with smsd" ;;
	esac
	# +CREG/+CEREG lines mean registration reports are on again (e.g. after the
	# modem restarted and before smsd re-sent its init); ^NDISDUN once per dial
	# is normal
	last=$(grep 'Unexpected input: +C' /srv/log/messages 2>/dev/null | tail -n 1 | cut -c5-19)
	say "last registration report smsd saw: ${last:-none in /srv/log/messages}"
	[ "$problems" -eq 0 ] && say "OK: no clashes" || say "$problems problem(s)"
	return "$problems"
}

apply() {
	modem_present || die "modem $VID:$PID not on USB"
	[ "$(ifnum ttyUSB3)" = 0b ] && [ "$(ifnum ttyUSB4)" = 0d ] ||
		die "ttyUSB3/ttyUSB4 aren't interfaces 0b/0d: the numbering has moved, so the plan's names are wrong. Changed nothing."
	[ -f "$SMSD_CONF" ] || die "no $SMSD_CONF"
	[ -z "$(uci changes network)" ] || die "network has uncommitted changes (uci changes network); won't commit someone else's edits"
	b=/root/backups/modem-ports-$(date +%Y%m%d-%H%M%S)-$$
	mkdir -p "$b" && cp -p /etc/config/network "$SMSD_CONF" "$b/" || die "backup to $b failed"
	say "backup: $b"

	if [ "$(uci -q get network.wwan.device)" != "$WANT_WWAN" ] || [ -z "$(uci -q get network.wwan.delay)" ]; then
		uci set network.wwan.device="$WANT_WWAN"
		# delay = seconds ncm.sh keeps retrying "who made this modem" (AT+CGMI)
		# before giving up; without it, one failed read fails the whole dial
		[ -n "$(uci -q get network.wwan.delay)" ] || uci set network.wwan.delay=5
		uci commit network
		say "wwan: device $WANT_WWAN, delay $(uci -q get network.wwan.delay); re-dialling"
		ifup wwan
		i=0 up=0
		while [ $i -lt 90 ]; do
			sleep 2; i=$((i + 2))
			s=$(ifstatus wwan 2>/dev/null)
			u=$(echo "$s" | sed -n 's/.*"uptime": \([0-9]*\).*/\1/p')
			# up, and a fresh session (not the old one before the re-dial)
			echo "$s" | grep -q '"up": true' && [ -n "$u" ] && [ "$u" -le "$i" ] && { up=1; break; }
		done
		[ $up = 1 ] && say "wwan: up again after ${i}s" ||
			warn "wwan not up after ${i}s: check 'logread -e netifd'; the old config is in $b"
	else
		say "wwan: already on $WANT_WWAN"
	fi

	if [ "$(readlink -f "$(smsd_device)")" != "$WANT_SMSD" ] || [ "$(smsd_init)" != "$SMSD_INIT" ]; then
		sed -i -e "s|^\([[:space:]]*device[[:space:]]*=\).*|\1 $WANT_SMSD|" \
			-e "s|^\([[:space:]]*init[[:space:]]*=\).*|\1 $SMSD_INIT|" "$SMSD_CONF"
		n0=$(logread -e smsd | wc -l)
		/etc/init.d/smstools3 restart
		sleep 15
		say "smsd: device $(smsd_device), init '$(smsd_init)'; restarted. Its log since:"
		# hide the SIM's identifiers and any phone number
		logread -e smsd | tail -n +$((n0 + 1)) | sed -E 's/(IMEI|IMSI): .*/\1: <hidden>/; s/[0-9]{9,}/<num>/g'
		logread -e smsd | tail -n +$((n0 + 1)) | grep -q 'did not accept' &&
			warn "smsd: the modem refused an init string (see logread -e smsd)"
	else
		say "smsd: already on $WANT_SMSD with init '$SMSD_INIT'"
	fi
	say ""
	status
}

case "${1:-status}" in
status) status ;;
apply) apply ;;
*) die "usage: $0 [status|apply]" ;;
esac
