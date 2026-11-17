#!/bin/sh
# Runs ON THE ROUTER (OpenWrt 25.x, Asterisk 23, BusyBox ash).
#
# Asterisk's HEP capture: a copy of every SIP message the PBX sends or
# receives, wrapped in a UDP packet and sent to a capture tool on the LAN
# (sngrep, Homer). For learning and debugging SIP (doc 12 §7.1).
#
#   status        what's configured and loaded (read-only; the default)
#   on IP:PORT    switch it on, sending to IP:PORT (an address, not a name)
#   off           switch it off again
#
#   /usr/bin/ssh root@<router> 'sh -s -- on 192.168.1.10:9060' < scripts/router_hep.sh
#   then on that PC:   sngrep -L udp:0.0.0.0:9060          (no root needed)
#   if nothing arrives, let UDP 9060 from the router through the PC's firewall:
#   sudo ufw allow from <router-ip> to any port 9060 proto udp
#
# The trap this script exists for: OpenWrt builds pjproject (Asterisk's SIP
# stack) with the stock limit of 32 PJSIP modules; Asterisk's own bundled copy
# raises it to 38. With this router's SIP modules the table is already full,
# so res_hep_pjsip loads, shows "Running", and captures nothing: the only sign
# is "Unable to register module HEPv3 Logging Module" in Asterisk's log. So
# "on" gives HEP the slot of res_pjsip_history (the "pjsip set history" debug
# buffer, which HEP + sngrep replaces), and checks the log for that error.
#
# What "on" changes, after copying both files to /root/backups/hep-<time>-<pid>/:
#   modules.conf  comments out "noload = res_hep.so" and "noload = res_hep_pjsip.so",
#                 adds "noload = res_pjsip_history.so"; all marked, so "off"
#                 restores the file exactly
#   hep.conf      enabled = yes, capture_address = IP:PORT, and comments out
#                 capture_password: HEP carries it in clear in every packet, so
#                 it protects nothing, and sngrep doesn't need one
# then swaps the modules in the running Asterisk: no restart, no dropped
# calls. Both files are package conffiles; once they differ from the package,
# a LuCI/sysupgrade backup includes them (check with sysupgrade -l), so take a
# fresh backup afterwards if you want the setting to survive a reset.
#
# Only SIP is captured. The RJ11 phone (extension 100) reaches Asterisk over
# AudioSocket, not SIP, so a call between the handset and 600/601/800 shows
# nothing; a call between the handset and a SIP phone shows the SIP leg only.
#
# Testing on copies: AST_ETC=<dir with modules.conf and hep.conf>
# BACKUP_DIR=<dir> NO_RELOAD=1 (touches neither Asterisk nor sysupgrade).

mode=${1:-status}
[ $# -gt 0 ] && shift

E=${AST_ETC:-/etc/asterisk}
M=$E/modules.conf
H=$E/hep.conf
MARK='; router_hep.sh: HEP on'
HIST='noload = res_pjsip_history.so ; router_hep.sh: frees a PJSIP module slot for HEP'
REGERR='Unable to register module HEPv3'
B=

die() { echo "router_hep: $*" >&2; exit 1; }
live() { [ -z "$NO_RELOAD" ] && asterisk -rx 'core show version' >/dev/null 2>&1; }
ast() { asterisk -rx "$1"; }
loaded() { ast "module show like $1" 2>/dev/null | grep -q "^$1[[:blank:]]"; }
logfile() {
	d=$(sed -n 's/^[[:blank:]]*astlogdir[[:blank:]]*=>[[:blank:]]*//p' /etc/asterisk/asterisk.conf 2>/dev/null | tail -1)
	for f in "${d:-/var/log/asterisk}/messages.log" "${d:-/var/log/asterisk}/messages"; do
		[ -f "$f" ] && { echo "$f"; return; }
	done
}
regerrs() { [ -n "$1" ] && grep -c "$REGERR" "$1"; true; }

backup() {
	# the PID keeps two runs in the same second from sharing (and overwriting) one backup
	B=${BACKUP_DIR:-/root/backups}/hep-$(date +%Y%m%d-%H%M%S)-$$
	mkdir -p "$B" && cp -p "$M" "$H" "$B"/ || die "backup to $B failed"
	echo "backup: $B"
}
restore() {
	cp -p "$B/modules.conf" "$M"; cp -p "$B/hep.conf" "$H"
	die "$*; both files restored from $B"
}

status() {
	echo "hep.conf ($H):"
	awk '/^[ \t]*;/ { next }
	     /^[ \t]*(enabled|capture_address|capture_password|capture_id|uuid_type)[ \t]*=/ {
		k = $0; sub(/[ \t]*=.*/, "", k); gsub(/[ \t]/, "", k)
		v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*;.*/, "", v)
		if (k == "capture_password") v = "(set; not shown)"
		printf "  %-17s %s\n", k, v }' "$H"
	grep -q '^[[:blank:]]*capture_password' "$H" || echo "  capture_password  (none)"
	echo "modules.conf ($M):"
	grep -E 'res_hep|res_pjsip_history' "$M" | sed 's/^/  /'
	[ -n "$NO_RELOAD" ] && return 0
	echo "running Asterisk:"
	if live; then
		ast 'module show like hep' | grep '^res_hep' | sed 's/^/  /'
		ast 'module show like res_pjsip_history' | grep '^res_pjsip_history' | sed 's/^/  /'
		loaded res_hep.so || echo "  (no HEP module loaded)"
	else
		echo "  Asterisk isn't running"
	fi
	echo "in the LuCI/sysupgrade backup:"
	sysupgrade -l 2>/dev/null | grep -E "/etc/asterisk/(modules|hep)\.conf" | sed 's/^/  /' |
		grep . || echo "  neither file (both unchanged from the package)"
}

on() {
	addr=${1:-}
	echo "$addr" | grep -Eq '^([0-9]{1,3}\.){3}[0-9]{1,3}:[0-9]{1,5}$' ||
		die "on needs IP:PORT, e.g. 192.168.1.10:9060 (got '$addr')"
	[ -f "$H" ] || die "$H missing; install it: apk add asterisk-res-hep asterisk-res-hep-pjsip"
	[ -f "$M" ] || die "$M missing"
	backup
	sed -i -e "s|^noload[[:blank:]]*=[[:blank:]]*res_hep\.so[[:blank:]]*\$|;noload = res_hep.so $MARK|" \
	       -e "s|^noload[[:blank:]]*=[[:blank:]]*res_hep_pjsip\.so[[:blank:]]*\$|;noload = res_hep_pjsip.so $MARK|" "$M"
	if ! grep -q '^noload[[:blank:]]*=[[:blank:]]*res_pjsip_history\.so' "$M"; then
		# right after "autoload=", inside [modules]; cat > keeps the file's owner and mode
		awk -v line="$HIST" '{ print } !done && /^autoload[ \t]*=/ { print line; done = 1 }
			END { if (!done) print line }' "$M" >"$M.hep.tmp" &&
			cat "$M.hep.tmp" >"$M" && rm -f "$M.hep.tmp"
	fi
	sed -i -e 's/^\([[:blank:]]*enabled[[:blank:]]*=[[:blank:]]*\)[A-Za-z]*/\1yes/' \
	       -e "s/^\([[:blank:]]*capture_address[[:blank:]]*=[[:blank:]]*\)[^[:blank:];]*/\1$addr/" \
	       -e 's/^\([[:blank:]]*\)capture_password/\1;capture_password/' "$H"
	grep -Eq '^[[:blank:]]*enabled[[:blank:]]*=[[:blank:]]*yes' "$H" &&
		grep -Eq "^[[:blank:]]*capture_address[[:blank:]]*=[[:blank:]]*$addr([[:blank:];]|\$)" "$H" ||
		restore "hep.conf has no plain 'enabled =' / 'capture_address =' lines; edit it by hand"
	grep -Eq '^[[:blank:]]*noload[[:blank:]]*=[[:blank:]]*res_hep(_pjsip)?\.so' "$M" &&
		restore "modules.conf still has a noload line for res_hep; edit it by hand"

	if live; then
		log=$(logfile); n0=$(regerrs "$log")
		# a res_hep_pjsip that failed to register still shows "Running": start it afresh
		loaded res_hep_pjsip.so && ast 'module unload res_hep_pjsip.so'
		loaded res_pjsip_history.so && ast 'module unload res_pjsip_history.so'
		# order matters: res_hep_pjsip declines to load unless res_hep is
		# already loaded *and* enabled
		if loaded res_hep.so; then ast 'module reload res_hep.so'; else ast 'module load res_hep.so'; fi
		ast 'module load res_hep_pjsip.so'
		sleep 1
		n1=$(regerrs "$log")
		if ! loaded res_hep.so || ! loaded res_hep_pjsip.so; then
			echo "router_hep: WARNING: the HEP modules didn't load; see Asterisk's log" >&2
		elif [ -z "$log" ]; then
			echo "router_hep: can't find Asterisk's messages log to confirm the SIP hook; look for '$REGERR'" >&2
		elif [ "${n1:-0}" -gt "${n0:-0}" ]; then
			echo "router_hep: FAIL: '$REGERR' in $log: PJSIP's module table is still full;" >&2
			echo "  noload another unused res_pjsip module (doc 12 §7.1)" >&2
		else
			echo "HEP hooked into PJSIP: each SIP message now also goes to $addr"
		fi
	elif [ -z "$NO_RELOAD" ]; then
		echo "Asterisk isn't running; HEP starts with it"
	fi
	status
}

off() {
	[ -f "$H" ] && [ -f "$M" ] || die "$H or $M missing"
	backup
	if live; then
		loaded res_hep_pjsip.so && ast 'module unload res_hep_pjsip.so'
		loaded res_hep.so && ast 'module unload res_hep.so'
	fi
	sed -i -e "s|^;noload = \(res_hep[a-z_]*\.so\) $MARK\$|noload = \1|" \
	       -e '/^noload = res_pjsip_history\.so ; router_hep\.sh: /d' "$M"
	sed -i 's/^\([[:blank:]]*enabled[[:blank:]]*=[[:blank:]]*\)[A-Za-z]*/\1no/' "$H"
	# history gets its slot back, unless someone else had switched it off
	if live && ! grep -q '^noload[[:blank:]]*=[[:blank:]]*res_pjsip_history\.so' "$M"; then
		loaded res_pjsip_history.so || ast 'module load res_pjsip_history.so'
	fi
	status
}

case $mode in
	status) status ;;
	on) on "$@" ;;
	off) off ;;
	*) die "usage: router_hep.sh status | on IP:PORT | off" ;;
esac
