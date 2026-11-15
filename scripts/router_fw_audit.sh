#!/bin/sh
# Runs ON THE ROUTER (OpenWrt 25.x, firewall4, BusyBox ash). Read-only.
#
# Security check-up of the firewall and of the services sitting behind it.
#
#   ssh root@<router> 'sh -s' < scripts/router_fw_audit.sh
#
# One line per check: PASS, WARN (works, but one layer thinner than it
# should be), FAIL (exposed or broken), INFO (context, no verdict).
# Exit status 1 if anything FAILs. Changes nothing; the fixes are in
# router_fw_provision.sh. Background and the reasoning per check: doc 14.

fails=0
warns=0
say() {
	printf '%-4s  %s\n' "$1" "$2"
	case $1 in FAIL) fails=$((fails + 1)) ;; WARN) warns=$((warns + 1)) ;; esac
}
note() { printf '      %s\n' "$*"; }
section() { printf '\n== %s\n' "$*"; }

# section id of the firewall section of type $1 whose name is $2
sec_by_name() {
	for s in $(uci -X show firewall | sed -n "s/^firewall\.\([^.]*\)\.name='$2'\$/\1/p"); do
		[ "$(uci -q get "firewall.$s")" = "$1" ] && { echo "$s"; return; }
	done
}
zone_policy_ok() { [ "$1" = REJECT ] || [ "$1" = DROP ]; }

# router/PBX ports nobody on the WAN should reach: SSH, DNS, web, AMI, SIP,
# ARI, RTP
SENSITIVE="22 53 80 443 5038 5060 8088 10000-10199"
# does any port or range in $1 ("80", "80 443", "1000-2000", "1000:2000")
# overlap any port or range in $2?
port_hits() {
	for r in $(echo "$1" | tr ':,' '- '); do
		lo=${r%%-*}
		hi=${r##*-}
		case $lo$hi in '' | *[!0-9]*) continue ;; esac
		for p in $2; do
			[ "$lo" -le "${p##*-}" ] && [ "$hi" -ge "${p%%-*}" ] && return 0
		done
	done
	return 1
}

command -v fw4 >/dev/null || { echo "firewall4 (fw4) not found: this check is for OpenWrt 22.03+" >&2; exit 2; }

section "Firewall policy"
if fw4 check >/dev/null 2>&1; then say PASS "ruleset compiles (fw4 check)"; else say FAIL "fw4 check fails: the firewall may not be loaded at all"; fi

for o in input forward; do
	v=$(uci -q get firewall.@defaults[0].$o)
	if zone_policy_ok "$v"; then say PASS "default $o policy $v"
	else say FAIL "default $o policy is '${v:-unset}' (want REJECT): unzoned traffic gets in"; fi
done
[ "$(uci -q get firewall.@defaults[0].syn_flood)" = 1 ] && say PASS "SYN-flood rate limit on" || say WARN "SYN-flood rate limit off"

W=$(sec_by_name zone wan)
L=$(sec_by_name zone lan)
if [ -z "$W" ]; then
	say FAIL "no firewall zone named 'wan'"
else
	for o in input forward; do
		v=$(uci -q get "firewall.$W.$o")
		if zone_policy_ok "$v"; then say PASS "zone wan $o $v"
		else say FAIL "zone wan $o is '${v:-unset}': the internet side can open connections in"; fi
	done
	[ "$(uci -q get "firewall.$W.masq")" = 1 ] && say PASS "zone wan masquerades (NAT)" || say WARN "zone wan has no masquerade"
	list=$(uci -q get "firewall.$W.network")
	n=$(echo $list | wc -w)
	u=$(printf '%s\n' $list | sort -u | wc -l)
	if [ "$n" -gt "$u" ]; then
		say WARN "zone wan lists $n networks, only $u distinct (duplicate entries, harmless but confusing)"
	else
		say PASS "zone wan networks: $list"
	fi
fi

# OpenWrt's board script for this model stages "add wwan to zone wan" at
# every boot (preinit board_detect); the next LuCI Save & Apply commits it as
# another duplicate. router_fw_provision.sh drops it and adds an rc.local guard.
ch=$(uci -q changes firewall)
if [ -n "$ch" ]; then
	if [ "$(echo "$ch" | grep -vc "\.network+='wwan'\$")" = 0 ]; then
		say WARN "staged, uncommitted 'add wwan to zone wan' (from /etc/board.d/02_network at boot): the next LuCI Save & Apply adds a duplicate"
	else
		say INFO "firewall has unsaved changes: $(echo "$ch" | wc -l) line(s) (uci changes firewall)"
	fi
fi
if grep -q 'zlt-s12-pro' /etc/board.d/02_network 2>/dev/null; then
	grep -qF 'fw-provision: drop board.d duplicate wwan' /etc/rc.local 2>/dev/null &&
		say PASS "rc.local drops the boot-time duplicate 'wwan'" ||
		say WARN "no rc.local guard against the boot-time duplicate 'wwan' (router_fw_provision.sh adds it)"
fi

section "Which zone each live interface lands in"
inchain=$(nft list chain inet fw4 input 2>/dev/null)
for i in $(ubus call network.interface dump | jsonfilter -e '@.interface[@.up=true].interface'); do
	[ "$i" = loopback ] && continue
	st=$(ifstatus "$i")
	d=$(echo "$st" | jsonfilter -e '@.l3_device')
	proto=$(echo "$st" | jsonfilter -e '@.proto')
	a=$(echo "$st" | jsonfilter -e '@["ipv4-address"][0].address')
	[ -n "$d" ] || continue
	z=$(echo "$inchain" | grep -F "\"$d\"" | sed -n 's/.*jump input_\([A-Za-z0-9_]*\).*/\1/p' | head -1)
	case $proto in pppoe | pppoa | dhcp | dhcpv6 | ncm | qmi | mbim | 3g | modemmanager | wwan) uplink=1 ;; *) uplink=0 ;; esac
	if [ -z "$z" ]; then
		say WARN "$i ($d, $proto) is up but in no zone: default policy applies (rejected, not exposed)"
		continue
	fi
	zin=$(uci -q get "firewall.$(sec_by_name zone "$z").input")
	if [ "$uplink" = 1 ] && [ "$zin" = ACCEPT ]; then
		say FAIL "uplink $i ($d, $proto) is in zone '$z' whose input is ACCEPT: the WAN side can reach every router service"
	else
		say PASS "$i ($d${a:+ $a}) -> zone $z (input ${zin:-?})"
	fi
	case $a in
	100.6[4-9].* | 100.[7-9][0-9].* | 100.1[01][0-9].* | 100.12[0-7].*)
		note "$a is carrier-grade NAT space: unreachable from the open internet, reachable from the carrier's network" ;;
	10.*) note "$a is a carrier-private address: reachable from the carrier's network only" ;;
	esac
done

section "Holes punched from the WAN"
pf=0
for s in $(uci -X show firewall | sed -n "s/^firewall\.\([^.=]*\)=redirect\$/\1/p"); do
	[ "$(uci -q get "firewall.$s.src")" = wan ] || continue
	[ "$(uci -q get "firewall.$s.enabled")" = 0 ] && continue
	pf=1
	nm=$(uci -q get "firewall.$s.name")
	sp=$(uci -q get "firewall.$s.src_dport")
	to="$(uci -q get "firewall.$s.dest_ip"):$(uci -q get "firewall.$s.dest_port")"
	if port_hits "$sp" "$SENSITIVE"; then
		say FAIL "port forward '$nm' wan:$sp -> $to hits a router/PBX port (SSH, DNS, web, SIP 5060, RTP 10000-10199, ARI 8088, AMI 5038)"
	else
		say WARN "port forward '$nm' wan:$sp -> $to (make sure you still want it)"
	fi
done
[ $pf = 0 ] && say PASS "no port forwards from the WAN"

STOCK=" Allow-DHCP-Renew Allow-Ping Allow-IGMP Allow-DHCPv6 Allow-MLD Allow-ICMPv6-Input Allow-ICMPv6-Forward "
extra=0
for s in $(uci -X show firewall | sed -n "s/^firewall\.\([^.=]*\)=rule\$/\1/p"); do
	[ "$(uci -q get "firewall.$s.src")" = wan ] || continue
	[ "$(uci -q get "firewall.$s.target")" = ACCEPT ] || continue
	[ "$(uci -q get "firewall.$s.enabled")" = 0 ] && continue
	nm=$(uci -q get "firewall.$s.name")
	case $STOCK in *" $nm "*) continue ;; esac
	dst=$(uci -q get "firewall.$s.dest")
	dp=$(uci -q get "firewall.$s.dest_port")
	pr=$(uci -q get "firewall.$s.proto")
	extra=1
	case $nm in
	Allow-IPSec-ESP | Allow-ISAKMP)
		say WARN "stock rule '$nm' forwards unsolicited ${pr}${dp:+/$dp} from the WAN to LAN hosts"
		note "VPN clients behind NAT don't need it (their replies are already 'established');"
		note "it only matters for packets addressed straight to a LAN IP, which only the ISP's next hop or the modem can send" ;;
	*)
		if [ -z "$dst" ] && port_hits "${dp:-0-65535}" "$SENSITIVE"; then
			say FAIL "rule '$nm' opens ${pr:-any}/${dp:-all ports} on the router itself to the WAN"
		else
			say WARN "rule '$nm' accepts ${pr:-any}/${dp:-all} from the WAN${dst:+ to zone $dst}"
		fi ;;
	esac
done
[ $extra = 0 ] && say PASS "only the stock DHCP/ICMP/IGMP rules accept anything from the WAN"

rej=$(nft list chain inet fw4 reject_from_wan 2>/dev/null | sed -n 's/.*counter packets \([0-9]*\).*/\1/p' | head -1)
up=$(cut -d. -f1 /proc/uptime)
[ -n "$rej" ] && say INFO "$rej packets from the WAN rejected in the last $((up / 3600)) h $((up % 3600 / 60)) min (since boot)"

if ip -6 addr show scope global 2>/dev/null | grep -q inet6; then
	say INFO "global IPv6 present: LAN hosts get public addresses, so forward_wan is their only shield"
else
	say INFO "no global IPv6 address: nothing on the LAN is directly addressable over IPv6"
fi

section "What listens behind the firewall"
# netstat truncates the program column ("uci.conf -g da" for nginx), so take
# the PID from it and read the real name from /proc
netstat -tulnp 2>/dev/null | awk '$1 ~ /^(tcp|udp)/ { split($4, a, ":"); port = a[length(a)];
	addr = substr($4, 1, length($4) - length(port) - 1); pid = "-";
	for (i = 5; i <= NF; i++) if ($i ~ /^[0-9]+\//) { pid = $i; sub(/\/.*/, "", pid); break }
	if (addr == "0.0.0.0" || addr == "::") print $1 "/" port, pid }' |
	awk '!seen[$1]++' |
	while read -r pp pid; do
		prog=$(cat "/proc/$pid/comm" 2>/dev/null)
		say INFO "$pp (${prog:-?}) listens on all addresses: the firewall alone keeps it off the WAN"
	done

section "SSH (dropbear)"
D=$(uci -X show dropbear 2>/dev/null | sed -n "s/^dropbear\.\([^.=]*\)=dropbear\$/\1/p" | head -1)
if [ -n "$D" ]; then
	ifc=$(uci -q get "dropbear.$D.Interface")
	[ -n "$ifc" ] && say PASS "SSH bound to interface $ifc" || say WARN "SSH listens on every interface (bind it to the LAN)"
	pa=$(uci -q get "dropbear.$D.PasswordAuth")
	keys=$(grep -cE '^(ssh-|ecdsa-|sk-)' /etc/dropbear/authorized_keys 2>/dev/null)
	if [ "$pa" = off ] || [ "$pa" = 0 ]; then
		say PASS "SSH is key-only ($keys key(s) in authorized_keys)"
	else
		say WARN "SSH accepts passwords: any LAN or Wi-Fi device can try to guess root's ($keys key(s) installed)"
	fi
fi
pw=$(awk -F: '$1 == "root" { print $2 }' /etc/shadow)
case $pw in
"") say FAIL "root has NO password: LuCI and SSH open to anyone on the LAN" ;;
'*' | '!'* | x) say WARN "root password login locked (LuCI needs a password)" ;;
*) say PASS "root password set (hash type \$$(echo "$pw" | cut -d'$' -f2)\$)" ;;
esac

section "Web (LuCI, reverse proxies)"
if [ -d /etc/nginx ] && pidof nginx >/dev/null; then
	for s in $(uci -X show nginx 2>/dev/null | sed -n "s/^nginx\.\([^.=]*\)=server\$/\1/p"); do
		inc=$(uci -q get "nginx.$s.include")
		ret=$(uci -q get "nginx.$s.return")
		[ -n "$ret" ] && continue # a redirect-only server serves nothing
		case " $inc " in
		*" restrict_locally "*) say PASS "nginx server '$s' only answers LAN/loopback sources (restrict_locally)" ;;
		*) say WARN "nginx server '$s' answers any source address" ;;
		esac
	done
	for f in /etc/nginx/conf.d/*.conf; do
		[ -f "$f" ] || continue
		grep -q 'server[[:space:]]*{' "$f" || continue
		sn=$(sed -n 's/^[[:space:]]*server_name[[:space:]]*\([^;]*\);.*/\1/p' "$f" | head -1)
		pp=$(sed -n 's/^[[:space:]]*proxy_pass[[:space:]]*\([^;]*\);.*/\1/p' "$f" | head -1)
		if grep -qE 'restrict_locally|^[[:space:]]*(allow|deny)[[:space:]]' "$f"; then
			say PASS "nginx ${f##*/} (${sn:-?}) has a source-address restriction"
		else
			say WARN "nginx ${f##*/} (${sn:-?}${pp:+ -> $pp}) answers any source address; add 'include restrict_locally;'"
		fi
	done
fi
if pidof uhttpd >/dev/null; then
	for l in $(uci -q get uhttpd.main.listen_http) $(uci -q get uhttpd.main.listen_https); do
		case $l in 127.* | \[::1\]*) say PASS "uhttpd on $l (loopback)" ;; *) say INFO "uhttpd on $l" ;; esac
	done
fi

if [ -d /etc/asterisk ]; then
	section "Asterisk"
	sip=$(netstat -tuln 2>/dev/null | awk '$4 ~ /:5060$/ { print $4 }' | sort -u | tr '\n' ' ' | sed 's/ $//')
	case " $sip " in
	"  ") say INFO "nothing listens on 5060 (Asterisk down?)" ;;
	*" 0.0.0.0:5060 "* | *" :::5060 "*) say WARN "SIP listens on all addresses: $sip" ;;
	*) say PASS "SIP only on $sip" ;;
	esac
	getv() { sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*\([^ ;]*\).*/\1/p" "/etc/asterisk/$1" 2>/dev/null | head -1; }
	ami=$(getv manager.conf enabled)
	amib=$(getv manager.conf bindaddr)
	case $ami in
	yes | true | 1)
		if [ "$amib" = 127.0.0.1 ]; then say PASS "AMI on 127.0.0.1 only"
		else say FAIL "AMI (full PBX control) enabled on ${amib:-0.0.0.0}"; fi ;;
	*) say PASS "AMI disabled" ;;
	esac
	hen=$(getv http.conf enabled)
	hb=$(getv http.conf bindaddr)
	case $hen in
	yes | true | 1)
		if [ "$hb" = 127.0.0.1 ]; then say PASS "Asterisk HTTP/ARI on 127.0.0.1 only (reached through nginx)"
		else say WARN "Asterisk HTTP/ARI bound to ${hb:-all addresses}"; fi ;;
	*) say PASS "Asterisk HTTP/ARI disabled" ;;
	esac
fi

section "Optional services"
if [ -f /etc/config/upnpd ]; then
	if [ "$(uci -q get upnpd.config.enabled)" = 1 ]; then
		say WARN "UPnP/NAT-PMP enabled: any LAN device can open WAN ports without asking"
	else
		say PASS "UPnP installed but disabled"
	fi
fi
if [ -x /etc/init.d/snmpd ] && /etc/init.d/snmpd enabled; then
	say WARN "snmpd enabled on $(uci -q get snmpd.@agent[0].agentaddress) (default community strings are public)"
fi

section "Survives a factory reset (sysupgrade backup)"
kept=$(sysupgrade -l 2>/dev/null)
for f in /etc/config/firewall /etc/config/dropbear /etc/dropbear/authorized_keys /etc/nginx/restrict_locally $(ls /etc/nginx/conf.d/*.conf 2>/dev/null); do
	echo "$kept" | grep -qx "$f" && say PASS "backup keeps $f" || say WARN "backup does NOT keep $f (add it to /etc/sysupgrade.conf)"
done
echo "$kept" | grep -qE 'privkey|pjsip_secrets' &&
	say INFO "the backup holds TLS private keys and SIP passwords: store the .tar.gz like a password"

printf '\n%d FAIL, %d WARN\n' "$fails" "$warns"
[ "$fails" = 0 ]
