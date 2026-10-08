#!/bin/sh
# Runs ON THE ROUTER. Streams one modem MTD partition to stdout.
# Usage: MTD=<n> sh -s < this-script   (stdout = raw partition image)
i=${MTD:-1}
# Hold this adb shell open in background for the whole transfer — the 2014
# adb server kills modem-side children when their shell exits.
adb shell "python /mnt/userdata/tools/modem_send.py /dev/mtdblock$i 5530" </dev/null >/dev/null 2>&1 &
HOLDPID=$!
t=0
while [ $t -lt 20 ]; do
	adb shell netstat -tln | grep -q ':5530' && break
	sleep 1
	t=$((t+1))
done
if ! adb shell netstat -tln | grep -q ':5530'; then
	echo "listener for mtd$i never came up" >&2
	kill $HOLDPID 2>/dev/null
	exit 1
fi
exec nc 192.168.9.2 5530
