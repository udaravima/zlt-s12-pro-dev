#!/bin/sh
# Runs ON THE ROUTER (OpenWrt 25.x, firewall4, BusyBox ash).
#
# Puts the firewall, and the services that sit behind it, into the state
# router_fw_audit.sh checks for. Meant for after a factory reset, or any time
# the audit WARNs. Safe to re-run: each step compares first, changes only what
# differs, and says what it did. A second run prints "nothing to do".
#
#   ssh root@<router> 'sh -s' < scripts/router_fw_provision.sh             # dry run: print the plan
#   ssh root@<router> 'sh -s -- --apply' < scripts/router_fw_provision.sh  # do it
#
# Options (after --):
#   --apply               write the changes (default: dry run, nothing written)
#   --keep-ssh-passwords  leave SSH password login on
#   --ssh-any-interface   leave SSH listening on every interface
#   --keep-ipsec          keep the stock IPsec passthrough rules
#   --no-dns-policy       don't (re)create the DNS-intercept / DoT-block rules
#   --no-boot-fix         don't add the rc.local guard against the boot-time
#                         duplicate 'wwan' (see the preflight comment)
# Environment:
#   WAN_IFACES  networks that belong in zone wan. Default below; names that
#               don't exist in /etc/config/network are skipped, entries
#               already in the zone are always kept.
#   UCI_CONFDIR, NGINX_DIR, AUTH_KEYS, RC_LOCAL
#               test against copies instead of the live files; setting
#               UCI_CONFDIR also implies NO_RELOAD=1 (doc 14 §4). Trap: uci
#               always also reads the live staging folder /tmp/.uci, so a test
#               run sees (but can't revert) the live boot-time 'wwan' line and
#               repeats that step every time. Live runs converge.
#
# Never touches: zone lan, port forwards (reported, not removed), Asterisk's
# own config (slic-asterisk-setup owns it), interface up/down state.
# Before --apply writes anything it copies every file it may change to
# /root/backups/fw-provision-<time>/.

APPLY=0
SSH_KEYS=1
SSH_LAN=1
DROP_IPSEC=1
DNS_POLICY=1
BOOT_FIX=1
for a; do
	case $a in
	--apply) APPLY=1 ;;
	--keep-ssh-passwords) SSH_KEYS=0 ;;
	--ssh-any-interface) SSH_LAN=0 ;;
	--keep-ipsec) DROP_IPSEC=0 ;;
	--no-dns-policy) DNS_POLICY=0 ;;
	--no-boot-fix) BOOT_FIX=0 ;;
	*) echo "unknown option: $a (see the header of this script)" >&2; exit 2 ;;
	esac
done

WAN_IFACES=${WAN_IFACES:-"slt_internet_wan wwan wwan2 slt_voip_wan slt_iptv_wan voip_br"}
NGINX_DIR=${NGINX_DIR:-/etc/nginx}
AUTH_KEYS=${AUTH_KEYS:-/etc/dropbear/authorized_keys}
RC_LOCAL=${RC_LOCAL:-/etc/rc.local}
NO_RELOAD=${NO_RELOAD:-0}
if [ -n "${UCI_CONFDIR:-}" ]; then
	mkdir -p "$UCI_CONFDIR/.save"
	U="uci -c $UCI_CONFDIR -t $UCI_CONFDIR/.save"
	CONF=$UCI_CONFDIR
	NO_RELOAD=1
	BK=$UCI_CONFDIR/.backup-$(date +%Y%m%d-%H%M%S)
else
	U=uci
	CONF=/etc/config
	BK=/root/backups/fw-provision-$(date +%Y%m%d-%H%M%S)
fi

die() { echo "ERROR: $*" >&2; exit 1; }
nplan=0
plan() {
	nplan=$((nplan + 1))
	if [ $APPLY = 1 ]; then echo "  do    $*"; else echo "  plan  $*"; fi
}
skip() { echo "  skip  $*"; }
# run a command only with --apply
run() { if [ $APPLY = 1 ]; then "$@"; fi; }

sec_by_name() { # type name
	for s in $($U -X show firewall | sed -n "s/^firewall\.\([^.]*\)\.name='$2'\$/\1/p"); do
		[ "$($U -q get "firewall.$s")" = "$1" ] && { echo "$s"; return; }
	done
}
ensure_opt() { # section option value label
	cur=$($U -q get "firewall.$1.$2")
	[ "$cur" = "$3" ] && return
	plan "$4: $2 '${cur:-unset}' -> '$3'"
	run $U set "firewall.$1.$2=$3"
}
ensure_policy() { # section option label   (DROP is stricter than REJECT: keep it)
	cur=$($U -q get "firewall.$1.$2")
	case $cur in REJECT | DROP) return ;; esac
	ensure_opt "$1" "$2" REJECT "$3"
}
ensure_named() { # type name option=value...
	t=$1
	n=$2
	shift 2
	s=$(sec_by_name "$t" "$n")
	if [ -z "$s" ]; then
		plan "add $t '$n' ($*)"
		if [ $APPLY = 1 ]; then
			s=$($U add firewall "$t")
			$U set "firewall.$s.name=$n"
			for kv; do $U set "firewall.$s.$kv"; done
		fi
		return
	fi
	for kv; do ensure_opt "$s" "${kv%%=*}" "${kv#*=}" "$t '$n'"; done
}

# ---- preflight -------------------------------------------------------------
command -v fw4 >/dev/null || die "firewall4 (fw4) not found: this script is for OpenWrt 22.03+"
[ $APPLY = 1 ] || echo "dry run: nothing is written (add --apply to do it)"

# OpenWrt's board script for this model (/etc/board.d/02_network, branch
# tozed,zlt-s12-pro) runs `uci add_list firewall.@zone[1].network='wwan'` on
# EVERY boot: preinit calls board_detect just to pick LED behaviour. The add is
# staged, never committed, so the next LuCI "Save & Apply" (any page) commits
# it and zone wan gains one more duplicate 'wwan'. Once wwan is committed,
# that staged change is pure noise: drop it. On a fresh install (wwan not yet
# committed) it is the only thing that puts wwan in zone wan: keep it.
BOOT_ARTIFACT="\.network+='wwan'\$"
wwan_committed() { grep -q "list network 'wwan'" "$CONF/firewall"; }
only_boot_artifact() { # staged firewall changes are exactly the board.d add
	ch=$($U changes firewall 2>/dev/null)
	[ -n "$ch" ] && [ "$(echo "$ch" | grep -vc "$BOOT_ARTIFACT")" = 0 ]
}
if only_boot_artifact && wwan_committed; then
	plan "drop the staged duplicate 'wwan' that OpenWrt's board script adds at every boot"
	run $U revert firewall
fi
for c in firewall dropbear upnpd; do
	[ -f "$CONF/$c" ] || continue
	[ $c = firewall ] && only_boot_artifact && wwan_committed && continue # handled above
	[ -z "$($U changes "$c" 2>/dev/null)" ] ||
		die "'$c' has unsaved changes (LuCI 'Unsaved Changes'?): save or revert them first, or they'd be committed with these"
done
W=$(sec_by_name zone wan)
LZ=$(sec_by_name zone lan)
[ -n "$W" ] || die "no firewall zone named 'wan'"
[ -n "$LZ" ] || die "no firewall zone named 'lan'"
LAN_IF=$($U -q get "firewall.$LZ.network" | awk '{ print $1 }')

if [ $APPLY = 1 ]; then
	mkdir -p "$BK" || die "cannot create $BK"
	for c in firewall dropbear upnpd; do [ -f "$CONF/$c" ] && cp -p "$CONF/$c" "$BK/"; done
	[ -d "$NGINX_DIR/conf.d" ] && cp -pr "$NGINX_DIR/conf.d" "$BK/nginx-conf.d"
	echo "backup: $BK"
fi

# ---- 1. policies -----------------------------------------------------------
echo "== firewall policies"
DF=$($U -X show firewall | sed -n "s/^firewall\.\([^.=]*\)=defaults\$/\1/p" | head -1)
[ -n "$DF" ] || die "no defaults section in firewall config"
ensure_policy "$DF" input defaults
ensure_policy "$DF" forward defaults
ensure_opt "$DF" syn_flood 1 defaults
ensure_policy "$W" input "zone wan"
ensure_policy "$W" forward "zone wan"
ensure_opt "$W" masq 1 "zone wan"
ensure_opt "$W" mtu_fix 1 "zone wan"

# ---- 2. zone wan membership -------------------------------------------------
echo "== zone wan networks"
cur=$($U -q get "firewall.$W.network")
lanlist=" $($U -q get "firewall.$LZ.network") "
want=""
for n in $cur $WAN_IFACES; do
	case " $want " in *" $n "*) continue ;; esac
	case " $cur " in
	*" $n "*) ;;                                    # already listed: keep
	*) [ "$($U -q get "network.$n")" = interface ] || continue ;;
	esac
	case $lanlist in
	*" $n "*) skip "$n is in zone lan; not moving it (decide by hand: an uplink in lan is exposed)"; continue ;;
	esac
	want="$want $n"
done
want=${want# }
if [ "$want" != "$cur" ]; then
	plan "zone wan: $(echo $cur | wc -w) entries -> $(echo $want | wc -w) ($want)"
	run $U delete "firewall.$W.network"
	for n in $want; do run $U add_list "firewall.$W.network=$n"; done
fi

# ---- 2b. stop the boot-time duplicate at its source ---------------------------
# Same guard as the preflight, run from rc.local at the end of every boot
# (rc.local is kept by the sysupgrade backup).
echo "== boot-time 'wwan' duplicate (OpenWrt board.d)"
RC_TAG="fw-provision: drop board.d duplicate wwan"   # must match the snippet's first line
if [ $BOOT_FIX = 1 ] && [ -f "$RC_LOCAL" ] && ! grep -qF "$RC_TAG" "$RC_LOCAL"; then
	plan "rc.local: drop the staged duplicate at each boot (only when wwan is already committed)"
	if [ $APPLY = 1 ]; then
		cp -p "$RC_LOCAL" "$BK/rc.local"
		snip=$RC_LOCAL.snippet.$$
		tmp=$RC_LOCAL.new.$$
		cat >"$snip" <<-'EOF'
		# fw-provision: drop board.d duplicate wwan (doc 14 §3).
		# /etc/board.d/02_network stages "add wwan to zone wan" on every boot;
		# once wwan is committed, committing it again only adds a duplicate.
		if grep -q "list network 'wwan'" /etc/config/firewall &&
		    [ "$(uci -q changes firewall | grep -vc "\.network+='wwan'\$")" = 0 ]; then
		    uci -q revert firewall
		fi

		EOF
		# insert before the first bare "exit 0" (stock rc.local ends with one)
		if awk -v snip="$snip" '
			/^[[:space:]]*exit 0[[:space:]]*$/ && !done {
				while ((getline l < snip) > 0) print l
				done = 1
			}
			{ print }
			END { exit !done }' "$RC_LOCAL" >"$tmp"; then
			cat "$tmp" >"$RC_LOCAL" # keeps the file's owner and mode
		else
			echo "  WARNING: $RC_LOCAL has no 'exit 0' line; add the guard by hand (doc 14 §3)" >&2
		fi
		rm -f "$snip" "$tmp"
	fi
elif [ $BOOT_FIX = 0 ]; then
	skip "rc.local guard (--no-boot-fix)"
fi

# ---- 3. stock IPsec passthrough ---------------------------------------------
echo "== WAN-to-LAN passthrough rules"
if [ $DROP_IPSEC = 1 ]; then
	for n in Allow-IPSec-ESP Allow-ISAKMP; do
		s=$(sec_by_name rule "$n")
		[ -n "$s" ] || continue
		plan "remove stock rule $n (lets unsolicited WAN traffic through to LAN hosts; not needed behind NAT)"
		run $U delete "firewall.$s"
	done
else
	skip "IPsec passthrough rules (--keep-ipsec)"
fi

# ---- 4. DNS policy (the router answers all LAN DNS) --------------------------
echo "== DNS policy"
if [ $DNS_POLICY = 1 ]; then
	ensure_named redirect Intercept-DNS-to-router \
		src=lan src_dport=53 dest_port=53 'proto=tcp udp' target=DNAT family=any
	ensure_named rule Block-external-DoT-853 \
		src=lan dest=wan dest_port=853 'proto=tcp udp' target=REJECT family=any
else
	skip "DNS intercept / DoT block (--no-dns-policy)"
fi

# ---- 5. port forwards: report only -------------------------------------------
for s in $($U -X show firewall | sed -n "s/^firewall\.\([^.=]*\)=redirect\$/\1/p"); do
	[ "$($U -q get "firewall.$s.src")" = wan ] || continue
	[ "$($U -q get "firewall.$s.enabled")" = 0 ] && continue
	skip "port forward '$($U -q get "firewall.$s.name")' wan:$($U -q get "firewall.$s.src_dport") (review it yourself; not removed)"
done

# ---- 6. UPnP -----------------------------------------------------------------
echo "== UPnP"
upnp_off=0
if [ -f "$CONF/upnpd" ] && [ "$($U -q get upnpd.config.enabled)" != 0 ]; then
	plan "disable UPnP/NAT-PMP (lets LAN devices open WAN ports unasked)"
	run $U set upnpd.config.enabled=0
	upnp_off=1
fi

# ---- 7. SSH --------------------------------------------------------------------
echo "== SSH (dropbear)"
D=$($U -X show dropbear 2>/dev/null | sed -n "s/^dropbear\.\([^.=]*\)=dropbear\$/\1/p" | head -1)
if [ -z "$D" ]; then
	skip "no dropbear section"
else
	dset() { # option value label
		cur=$($U -q get "dropbear.$D.$1")
		[ "$cur" = "$2" ] && return
		plan "SSH $3: $1 '${cur:-unset}' -> '$2'"
		run $U set "dropbear.$D.$1=$2"
	}
	if [ $SSH_KEYS = 1 ]; then
		keys=$(grep -cE '^(ssh-|ecdsa-|sk-)' "$AUTH_KEYS" 2>/dev/null)
		if [ "${keys:-0}" -gt 0 ]; then
			dset PasswordAuth off "key-only"
			dset RootPasswordAuth off "key-only"
		else
			skip "SSH key-only: $AUTH_KEYS has no key; turning passwords off now would lock you out"
		fi
	else
		skip "SSH key-only (--keep-ssh-passwords)"
	fi
	if [ $SSH_LAN = 1 ]; then
		if [ -n "$LAN_IF" ]; then dset Interface "$LAN_IF" "LAN only"
		else skip "SSH LAN-only: zone lan has no network"; fi
	else
		skip "SSH LAN-only (--ssh-any-interface)"
	fi
fi

# ---- 8. nginx sites: LAN/loopback sources only ------------------------------------
echo "== nginx sites"
nginx_changed=0
if [ -d "$NGINX_DIR/conf.d" ]; then
	for f in "$NGINX_DIR"/conf.d/*.conf; do
		[ -f "$f" ] || continue
		grep -q 'server[[:space:]]*{' "$f" || continue
		grep -qE 'restrict_locally|^[[:space:]]*(allow|deny)[[:space:]]' "$f" && continue
		if [ ! -f "$NGINX_DIR/restrict_locally" ]; then
			skip "${f##*/}: $NGINX_DIR/restrict_locally missing"
			continue
		fi
		if ! grep -q '^[[:space:]]*server_name[[:space:]]' "$f"; then
			skip "${f##*/}: no server_name line to anchor on; add 'include restrict_locally;' by hand"
			continue
		fi
		plan "nginx ${f##*/}: add 'include restrict_locally;' (answer LAN/loopback sources only)"
		run sed -i '/^[[:space:]]*server_name[[:space:]]/a\    include restrict_locally;' "$f"
		nginx_changed=1
	done
fi

# ---- commit and reload ---------------------------------------------------------
echo
if [ $nplan = 0 ]; then
	echo "nothing to do: already provisioned"
	[ $APPLY = 1 ] && rm -rf "$BK" # made by this run, holds only unchanged copies
	exit 0
fi
if [ $APPLY = 0 ]; then
	echo "$nplan change(s) planned. Re-run with --apply to make them."
	exit 0
fi

fw_changed=0
ssh_changed=0
[ -n "$($U changes firewall)" ] && fw_changed=1
[ -f "$CONF/dropbear" ] && [ -n "$($U changes dropbear)" ] && ssh_changed=1
for c in firewall dropbear upnpd; do
	[ -f "$CONF/$c" ] && [ -n "$($U changes "$c")" ] && $U commit "$c"
done

if [ "$NO_RELOAD" = 1 ]; then
	echo "committed to $CONF (test mode: no service reloaded)"
	exit 0
fi

if [ $fw_changed = 1 ]; then
	if ! fw4 check >/dev/null 2>&1; then
		cp -p "$BK/firewall" /etc/config/firewall
		die "new firewall config fails 'fw4 check'; restored $BK/firewall, firewall not reloaded"
	fi
	/etc/init.d/firewall reload >/dev/null 2>&1 && echo "firewall reloaded"
fi
if [ $nginx_changed = 1 ]; then
	if nginx -t >/dev/null 2>&1; then
		/etc/init.d/nginx reload && echo "nginx reloaded"
	else
		cp -p "$BK"/nginx-conf.d/*.conf "$NGINX_DIR/conf.d/"
		echo "WARNING: nginx -t failed with the change; restored conf.d from $BK" >&2
	fi
fi
if [ $upnp_off = 1 ] && [ -x /etc/init.d/miniupnpd ]; then
	/etc/init.d/miniupnpd stop
	/etc/init.d/miniupnpd disable
fi
if [ $ssh_changed = 1 ]; then
	/etc/init.d/dropbear reload
	echo "SSH reloaded. This session stays open: open a SECOND ssh session now and"
	echo "confirm you still get in before closing this one. Locked out? LuCI still"
	echo "works with the root password: System > Administration > SSH Access."
fi
echo "done. Check with: ssh root@<router> 'sh -s' < scripts/router_fw_audit.sh"
